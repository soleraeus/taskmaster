/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   administrator.c                                    :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: tnaton <marvin@42.fr>                      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2023/06/16 14:30:47 by tnaton            #+#    #+#             */
/*   Updated: 2023/07/25 18:35:50 by bdetune          ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

//#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "taskmaster.h"
#include <pthread.h>

extern char **environ;

char *getcmd(struct s_process *proc) {
		char *command = NULL;
		if (!access(proc->program->command, X_OK)) { // if no need to search w/ path (aka absolute path or relative
			command = strdup(proc->program->command);
		} else {
			char *envpath = strdup(getenv("PATH"));
			if (envpath) {
				char *path;
				path = strtok(envpath, ":");
				while (path) {// size of path + '/'    +    size of command   +   '\0'
					size_t size = (strlen(path) + 1 + strlen(proc->program->command) + 1);
					command = (char *)calloc(size, sizeof(char));
					if (!command) {
						break ;
					}
					strcat(command, path);
					strcat(command, "/");
					strcat(command, proc->program->command);
					if (!access(command, X_OK)) {
						break ;
					}
					free(command);
					command = NULL;
					path = strtok(NULL, ":");
				}
				free(envpath);
			} else {
				free(command);
				return (NULL);
			}
		}
		return (command);
}

void child_exec(struct s_process *proc) {
		// dup every standard stream to pipe
		dup2(proc->stdin[0], 0);
		dup2(proc->stdout[1], 1);
		dup2(proc->stderr[1], 2);

		// close ends of pipe which doesnt belong to us
		close(proc->stdin[1]);
		close(proc->stdout[0]);
		close(proc->stderr[0]);
		close(proc->com[0]);
		close(proc->com[1]);
	
		if (proc->program->env) {
			struct s_env*	begin;

			begin = proc->program->env;
			while (begin)
			{
				putenv(strdup(begin->value));
				begin = begin->next;
			}
		}
		char *command = getcmd(proc);

		if (!command) {
			printf("FATAL ERROR IN GETCMD WILL JOIN %p\n", (void *)&proc->handle);
		} else {
			printf("command %s\n", command);
			execve(command, proc->program->args, environ);
			perror("execve");
		}
		// one error dont leak fds
		printf("%p | %p", (void *)proc->handle, (void *)*(&(proc->handle)));
		close(proc->log);
		close(proc->stdin[0]);
		close(proc->stdout[1]);
		close(proc->stderr[1]);
		free(command);
		free(proc->name);
		exit(1);
}

int exec(struct s_process *process, int epollfd) {

	struct epoll_event out, err;

	if (pipe(process->stdin)) {
		return 1;
	}
	if (pipe(process->stdout)) {
		return 1;
	}
	if (pipe(process->stderr)) {
		return 1;
	}

	printf("in : %d|%d\nout : %d|%d\nerr : %d|%d\n", process->stdin[0], process->stdin[1], process->stdout[0], process->stdout[1], process->stderr[0], process->stderr[1]);

	// set listening to stdout
	out.events = EPOLLIN;
	out.data.fd = process->stdout[0];

	// set listening to stderr
	err.events = EPOLLIN;
	err.data.fd = process->stderr[0];

	// register out
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, process->stdout[0], &out) == -1) {
		perror("epoll_ctl");
	}

	// register err
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, process->stderr[0], &err) == -1) {
		perror("epoll_ctl");
	}

	// fork for exec
	if ((process->pid = fork()) < 0) {
		return 1;
	}
	if (!process->pid) {
		// child execve
		close(epollfd);
		child_exec(process);
	}

	// close the ends of the pipe which doesnt belong to us
	close(process->stdin[0]);
	close(process->stdout[1]);
	close(process->stderr[1]);

	// set start of process
	if (gettimeofday(&process->start, NULL)) {
		return 1;
	}

	// change to running
	process->status = STARTING;

	return 0;
}

bool should_start(struct s_process *process) {
	if (process->status == EXITED) {
		if (process->program->autorestart == ALWAYS) {
			return true;
		} else if (process->program->autorestart == ONERROR) {
			// check if exit status is expected or not
		} else {
			return false;
		}
	} else if (process->status == BACKOFF) {
		if (process->count_restart < process->program->startretries) {
			process->count_restart++;
			printf("RETRYING FOR THE %d TIME\n", process->count_restart);
			return true;
		} else {
			process->status = FATAL;
			return false;
		}
	}
	return false;
}

void closeall(struct s_process *process, int epollfd) {
	printf("%s exited\n", process->name);
	epoll_ctl(epollfd, EPOLL_CTL_DEL, process->stdout[0], NULL);
	epoll_ctl(epollfd, EPOLL_CTL_DEL, process->stderr[0], NULL);
	close(process->stdin[1]);
	close(process->stdout[0]);
	close(process->stderr[0]);
	waitpid(process->pid, NULL, -1);
}

void *administrator(void *arg) {
	struct s_process *process = (struct s_process *)arg;
	printf("Administrator for %s created\n", process->name);

	bool				start = false; // this bool serve for autostart and start signal sent by controller
	int					nfds = 0;
	struct epoll_event	events[3], in;
	int					epollfd = epoll_create(1);
	struct timeval		stop;  //
	stop.tv_sec = 0;           // NEED TO SET W/ gettimeofday WHEN CHANGING STATUS TO STOPPING
	stop.tv_usec = 0;          //

	if (process->program->autostart) {
		start = true;
	}

	in.events = EPOLLIN;
	in.data.fd = 0;
	(void)in;
	// ALL THIS IS TEMPORARY -- WILL USE PIPE WITH MAIN THREAD INSTEAD
	//epoll_ctl(epollfd, EPOLL_CTL_ADD, 0, &in);

	while (1) {
		if (process->status == EXITED || process->status == BACKOFF || start) {
			if (should_start(process) || start) {
				start = false;
				printf("starting %s\n", process->name);
				printf("with raw cmd %s\n", process->program->command);
				if (process->program->args) {
					printf("with args ");
					char **tmp = process->program->args;
					while (*tmp) {
						printf("%s ", *tmp);
						tmp++;
					}
					printf("\n");
				} else {
					printf("with args NULL\n");
				}
				if (exec(process, epollfd)) {
					printf("FATAL ERROR\n");
					return NULL;
				}
			} else {
				printf("%s could not restart, exiting\n", process->name);
				return NULL;
			}
		}
		if (process->status == STARTING) {
			struct timeval tmp;
			if (gettimeofday(&tmp, NULL)) {
				printf("FATAL ERROR\n");
				return NULL;
			}

			long long start_micro = ((process->start.tv_sec * 1000) + (process->start.tv_usec / 1000));
			long long tmp_micro = ((tmp.tv_sec * 1000) + (tmp.tv_usec / 1000));

			if (((long long)process->program->startsecs * 1000) <= (tmp_micro - start_micro)) {
				process->status = RUNNING;
				printf("NOW RUNNING\n");
				// need to epoll_wait 0 to return instantly
				tmp_micro = ((long long)process->program->startsecs * 1000);
				start_micro = 0;
			}
			printf("time to wait : %lld\n", ((long long)process->program->startsecs * 1000) - (tmp_micro - start_micro));
			if (((long long)process->program->startsecs * 1000) - (tmp_micro - start_micro) > INT_MAX) { // if time to wait is bigger than an int, we wait as much as possible and come again if we got no event
				nfds = epoll_wait(epollfd, events, 3, INT_MAX);
			} else {
				nfds = epoll_wait(epollfd, events, 3, (int)(((long long)process->program->startsecs * 1000) - (tmp_micro - start_micro)));
			}
			if (nfds) { // if not timeout
				printf("GOT EVENT\n");
				for (int i = 0; i < nfds; i++) {
				/* handles fds as needed */
					if (events[i].data.fd == process->stdout[0]) { // if process print in stdout
						char buf[4096];
						bzero(buf, 4096);
						if (read(process->stdout[0], buf, 4095) > 0) {
							printf("%s : >%s<\n", process->name, buf);
						} else {
							closeall(process, epollfd);
							process->status = BACKOFF;
							break ;
						}
					}
					if (events[i].data.fd == process->stderr[0]) { // if process print in stderr
						char buf[4096];
						bzero(buf, 4096);
						if (read(process->stderr[0], buf, 4095) > 0) {
							printf("%s ERROR : >%s<\n", process->name, buf);
						} else {
							closeall(process, epollfd);
							process->status = BACKOFF;
							break ;
						}
					}
				}
			} else if (((long long)process->program->startsecs * 1000) - (tmp_micro - start_micro) > INT_MAX) { // if timeout and not because time to wait is bigger than an int
				process->status = RUNNING;
				printf("%s IS NOW RUNNING\n", process->name);
			}
		} else if (process->status == RUNNING) {
			nfds = epoll_wait(epollfd, events, 3, -1); // busy wait for smth to do

			printf("number events : %d\n", nfds);
			for (int i = 0; i < nfds; i++) {
			/* handles fds as needed */
				printf("GOT EVENT\n");
				if (events[i].data.fd == process->stdout[0]) { // if process print in stdout
					char buf[4096];
					bzero(buf, 4096);
					if (read(process->stdout[0], buf, 4095) > 0) {
						printf("%s : >%s<\n", process->name, buf);
					} else {
						closeall(process, epollfd);
						process->status = EXITED;
						break ;
					}
				}
				if (events[i].data.fd == process->stderr[0]) { // if process print in stderr
					char buf[4096];
					bzero(buf, 4096);
					if (read(process->stderr[0], buf, 4095) > 0) {
						printf("%s ERROR : >%s<\n", process->name, buf);
					} else {
						closeall(process, epollfd);
						process->status = EXITED;
						break ;
					}
				}
				if (events[i].data.fd == 0) {
					printf("READ STDIN AND WRITING\n");
					char buf[4096];
					bzero(buf, 4096);
					if (read(0, buf, 4095) > 0) {
						printf("send : >%s<\n", buf);
						ssize_t ret =  (write(process->stdin[1], buf, strlen(buf)));
						printf("Wrote to pipe stdin %ld chars out of %ld\n", ret, strlen(buf));
					}
				}
			}
		} else if (process->status == STOPPING) {
			struct timeval tmp;
			if (gettimeofday(&tmp, NULL)) {
				printf("FATAL ERROR\n");
				return NULL;
			}

			long long start_micro = ((stop.tv_sec * 1000) + (stop.tv_usec / 1000));
			long long tmp_micro = ((tmp.tv_sec * 1000) + (tmp.tv_usec / 1000));

			if ((long long)(process->program->stopwaitsecs * 1000) <= (long long)(tmp_micro - start_micro)) {
				process->status = STOPPED;
				printf("FORCED STOPPED\n");
				// kill(process->pid, SIGKILL);
				// need to epoll_wait 0 to return instantly
				tmp_micro = (process->program->stopwaitsecs * 1000);
				start_micro = 0;
			}
			printf("time to wait : %lld\n", (process->program->stopwaitsecs * 1000) - (tmp_micro - start_micro));
			nfds = epoll_wait(epollfd, events, 3, (int)((process->program->stopwaitsecs * 1000) - (tmp_micro - start_micro)));
			if (nfds) { // if not timeout
				printf("GOT EVENT\n");
				for (int i = 0; i < nfds; i++) {
				/* handles fds as needed */
					if (events[i].data.fd == process->stdout[0]) { // if process print in stdout
						char buf[4096];
						bzero(buf, 4096);
						if (read(process->stdout[0], buf, 4095) > 0) {
							printf("%s : >%s<\n", process->name, buf);
						} else {
							closeall(process, epollfd);
							process->status = STOPPED;
							stop.tv_sec = 0;
							stop.tv_usec = 0;
							break ;
						}
					}
					if (events[i].data.fd == process->stderr[0]) { // if process print in stderr
						char buf[4096];
						bzero(buf, 4096);
						if (read(process->stderr[0], buf, 4095) > 0) {
							printf("%s ERROR : >%s<\n", process->name, buf);
						} else {
							closeall(process, epollfd);
							process->status = STOPPED;
							stop.tv_sec = 0;
							stop.tv_usec = 0;
							break ;
						}
					}
				}
			} else { // if timeout
				process->status = STOPPED;
				printf("FORCED STOPPED\n");
				// kill(process->pid, SIGKILL);
				stop.tv_sec = 0;
				stop.tv_usec = 0;
			}
		} else if (process->status == STOPPED || process->status == EXITED || process->status == FATAL) {
			nfds = epoll_wait(epollfd, events, 3, -1); // busy wait for main thread to send instruction

			for (int i = 0; i < nfds; i++) {
				// do shit
			}
		}
	}
	printf("Exiting administrator\n");
	return NULL;
}

/*
void test(void) {
	struct s_program prog;
	char *cmd = "bash";
	char *args[] = {cmd, "salut", NULL};
	char *args2[] = {"whoami", NULL};
	char *env[] = {"USER=pasmoilol", "TEST=42", NULL};

	prog.name = "bash";
	prog.command = cmd;
	prog.args = args;
	prog.numprocs = 1;
	prog.priority = 1;
	prog.autostart = true;
	prog.startsecs = 1;
	prog.startretries = 0;
	prog.autorestart = ONERROR;
	prog.exitcodes = NULL;
	prog.stopsignal = 0;
	prog.stopwaitsecs = 5;
	prog.stdoutlog = false;
	prog.stdoutlogpath = NULL;
	prog.stderrlog = false;
	prog.stderrlogpath = NULL;
	prog.env = env;
	prog.workingdir = "/usr";
	prog.umask = 0;
	prog.user = NULL;

	struct s_program prog2;

	prog2.name = "quisuisje";
	prog2.command = "whoami";
	prog2.args = args2;
	prog2.numprocs = 3;
	prog2.priority = 1;
	prog2.autostart = true;
	prog2.startsecs = 1;
	prog2.startretries = 0;
	prog2.autorestart = ONERROR;
	prog2.exitcodes = NULL;
	prog2.stopsignal = 0;
	prog2.stopwaitsecs = 5;
	prog2.stdoutlog = false;
	prog2.stdoutlogpath = NULL;
	prog2.stderrlog = false;
	prog2.stderrlogpath = NULL;
	prog2.env = env;
	prog2.workingdir = "/";
	prog2.umask = 0;
	prog2.user = NULL;


	struct s_program **lst = (struct s_program **)calloc(sizeof(struct s_program *), 3);

	lst[0] = &prog;
	lst[1] = &prog2;

	struct s_process proc;

	proc.name = "listing";
	proc.pid = 0;
	proc.status = STOPPED;
	proc.program = &prog;
	proc.count_restart = 0;
	bzero(proc.stdin, 2);
	bzero(proc.stdout, 2);
	bzero(proc.stderr, 2);
	create_process(lst);
	administrator(&proc);
}*/