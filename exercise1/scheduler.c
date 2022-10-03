#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/types.h>
#include "proc-common.h"
#include "request.h"
/* Compile-time parameters. */
#define SCHED_TQ_SEC 2                /* time quantum */
#define TASK_NAME_SZ 60               /* maximum size for a task's name */

// contains all PIDs of children
pid_t *pid;
//current hold the pid of running proc
int current, nproc, current_id;
// true if on children is dead
typedef enum { false, true } bool;
bool *dead;

/*
 * SIGALRM handler
 */
static void sigalrm_handler(int signum)
{
	if (signum != SIGALRM) {
		fprintf(stderr, "Internal error: Called for signum %d, not SIGALRM\n", signum);
		exit(1);
	}
	printf("\nALARM! %d seconds have passed. Send SIGSTOP to proc with PID:%d \n", SCHED_TQ_SEC, current);
	/* Setup the alarm again */
	kill(current, SIGSTOP);
}

/*
 * SIGCHLD handler
 */
static void sigchld_handler(int signum)
{
	pid_t p;
	int status;

	if (signum != SIGCHLD) {
		fprintf(stderr, "Internal error: Called for signum %d, not SIGCHLD\n",
		        signum);
		exit(1);
	}
	/*
	 * Something has happened to one of the children.
	 * We use waitpid() with the WUNTRACED flag, instead of wait(), because
	 * SIGCHLD may have been received for a stopped, not dead child.
	 *
	 * A single SIGCHLD may be received if many processes die at the same time.
	 * We use waitpid() with the WNOHANG flag in a loop, to make sure all
	 * children are taken care of before leaving the handler.
	 */
	for (;;) {
		p = waitpid(-1, &status, WUNTRACED | WNOHANG);
		if (p < 0) {
			perror("waitpid");
			exit(1);
		}
		if (p == 0)
			break;

		explain_wait_status(p, status);

		if (WIFEXITED(status) || WIFSIGNALED(status)) {
			/* A child has died */
			printf("Parent: Received SIGCHLD, child with PID:%d is dead.\n", current);
			dead[current_id] = true;
		}
		if (WIFSTOPPED(status)) {
			/* A child has stopped due to SIGSTOP/SIGTSTP, etc... */
			printf("Parent: Child has been stopped. Moving right along...\n");
		}
		//check next alive proc
		current_id = (current_id + 1) % nproc;
		current = pid[current_id];
		int i = 0; //escape from loop
		while (true)
		{
			if (!dead[current_id]) {
				if (alarm(SCHED_TQ_SEC) < 0) {
					perror("alarm");
					exit(1);
				}
				kill(current, SIGCONT);
				break;
			}
			current_id = (current_id + 1) % nproc;
			current = pid[current_id];
			i++;
			if (i > nproc) {
				printf("\nYAY! ALL PROCS ARE DEAD!\n");
				exit(0);
			}
		}

	}
}

/* Install two signal handlers.
 * One for SIGCHLD, one for SIGALRM.
 * Make sure both signals are masked when one of them is running.
*/
static void install_signal_handlers(void)
{
	sigset_t sigset;
	struct sigaction sa;
	/* struct sigaction {
	              void     (*sa_handler)(int);
	              void     (*sa_sigaction)(int, siginfo_t *, void *);
	              sigset_t   sa_mask;
	              int        sa_flags;
	          };
	*/
	sa.sa_handler = sigchld_handler;
	//sa_handler specifies the action to be associated with signum and may be SIG_DFL for the default action, SIG_IGN to ignore this signal, or a
	//  pointer to a signal handling function.  This function receives the signal number as its only argument.
	sa.sa_flags = SA_RESTART;
	// Provide behavior compatible with BSD signal semantics by making certain system calls restartable across signals.  This  flag  is
	// meaningful only when establishing a signal handler.  See signal(7) for a discussion of system call restarting.
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGCHLD);
	sigaddset(&sigset, SIGALRM);
	sa.sa_mask = sigset;
	if (sigaction(SIGCHLD, &sa, NULL) < 0) {
		perror("sigaction: sigchld");
		exit(1);
	}
	sa.sa_handler = sigalrm_handler;
	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		perror("sigaction: sigalrm");
		exit(1);
	}
	/*
	 * Ignore SIGPIPE, so that write()s to pipes
	 * with no reader do not result in us being killed,
	 * and write() returns EPIPE instead.
	 */
	if (signal(SIGPIPE, SIG_IGN) < 0) {
		perror("signal: sigpipe");
		exit(1);
	}
}

int main(int argc, char *argv[])
{
	//	ID from 1 to N.
	// PID[] from 0 to N-1.
	int i;
	nproc = argc - 1;
	if (nproc == 0) {
		fprintf(stderr, "Scheduler: No tasks. Exiting...\n");
		exit(1);
	}
	else
		printf("I AM %s. I HAVE TO GIVE BIRTH TO %d PROCESSES \n", argv[0] , nproc);
	/*
	 * For each of argv[1] to argv[argc - 1],
	 * create a new child process, add it to the process list.
	 */
	pid = malloc(nproc * sizeof(pid_t));
	dead = malloc(nproc * sizeof(bool));
	for (i = 0; i < nproc; ++i)
	{
		dead[i] = false;
		pid[i] = fork();
		if (pid[i] < 0) {
			perror("error fork");
			exit(1);
		}
		if (pid[i] == 0) {
			printf("No.%d child process created with PID: %d \n", i, getpid());
			char *newargv[] = { argv[i + 1], NULL, NULL, NULL };
			char *newenviron[] = { NULL };
			raise(SIGSTOP);
			// AFTER SIGCONT I HAVE TO EXECVE
			printf("No.%d with PID:%d\n 	About to replace myself with the executable %s...\n", i, getpid(),
			       argv[i + 1]);
			execve(argv[i + 1], newargv, newenviron);

			/* execve() only returns on error */
			perror("execve");
			exit(1);
		}
	}
	/* Wait for all children to raise SIGSTOP before exec()ing. */
	wait_for_ready_children(nproc);
	//show_pstree(getpid());
	/* Install SIGALRM and SIGCHLD handlers. */
	install_signal_handlers();
	current = pid[0];
	current_id = 0;
	// Setup the alarm
	if (alarm(SCHED_TQ_SEC) < 0) {
		perror("alarm");
		exit(1);
	}
	kill(pid[0], SIGCONT);

	/* loop forever  until we exit from inside a signal handler. */
	while (pause())
		;

	/* Unreachable */
	fprintf(stderr, "Internal error: Reached unreachable point\n");
	return 1;
}
