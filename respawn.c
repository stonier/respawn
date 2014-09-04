/******************************************************************************
 **                                Respawn                                   **
 ******************************************************************************/

/*****************************************************************************
 *
 *  Process spawner utility for Linux
 *  Copyright (C) 2003 Thomas Horsten <thomas@horsten.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ****************************************************************************/



#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <getopt.h>
#include <stdlib.h>

/* Global program options, set by parse_arguments */
char *pidfile =0;
char **exe_vptr =0;
char *cwd =0;
char *logfile =0;
int overwrite_log=0;
int quiet_log=0;
int sleeptime=10;
enum {Undef,Start,Stop,Restart} runmode=Undef;

/* Globals used by getopt */
extern int opterr;

/* Globals used by signal handlers */
sig_atomic_t terminated=0;
sig_atomic_t restart_requested=0;

/* Show usage information */
void show_help(const char *cmdname)
{
  printf("Usage:\n"
         "%s [OPTIONS]... start FILE [ARGS]...\n"
         "       Executes FILE in the background, automatically restarting it if it\n"
         "       dies, optionally saving its standard output and standard error.\n"
         "\n"
         "%s [OPTIONS]... stop\n"
         "       Stop a program, by signalling the watching process.\n"
         "\n"
         "%s [OPTIONS]... restart\n"
         "       Cause the program to be reloaded by signalling the watching process\n"
         "\n"
         "  -d, --chdir=DIR      Change to the specified directory before starting.\n"
         "  -l, --logfile=FILE   Log program's standard output and standard error\n"
         "                       to FILE (only useful when starting). If not specified,\n"
         "                       output will go to /dev/null.\n"
         "  -q, --quietlog       Don't log the program's standard output and error,\n"
         "                       only spawn and exit messages.\n"
         "                       to FILE (only useful when starting). If not specified,\n"
         "                       output will go to /dev/null.\n"
         "  -o, --truncate-log   Truncate the logfile. Otherwise, append to it.\n"
         "  -p, --pidfile=FILE   Save the PID of the watcher process to FILE. If not\n"
         "                       specified, the PID file is not saved and stop or\n"
         "                       restart will not work.\n"
         "  -t, --delay=VAL      Delay for VAL seconds before respawning program when\n"
         "                       it has terminated (default: 10).\n"
         "\n"
         "Report bugs to <thomas@horsten.com>.\n"
         , cmdname,cmdname,cmdname);
}

/* Write to PID file, if one was specified */
void writepidfile(pid_t pid)
{
  FILE *f;

  if (!pidfile)
    return;

  f=fopen(pidfile, "w");
  if (f==NULL) {
    fprintf(stderr, "Warning: Couldn't write pidfile: %m\n");
    return;
}
  fprintf(f, "%d\n", pid);
  fclose(f);
  return;
}

/* Get PID from pidfile, if specified and found */
pid_t getpidfilepid()
{
  pid_t pid=0;
  if (pidfile)
{
      FILE *f=fopen(pidfile, "r");
      if (f!=NULL) {
        int v;
        if (fscanf(f,"%d",&v)==1) {
          pid=v;
}
        fclose(f);
}
}
  return pid;
}

/* Delete PID file if we have one */
void rmpidfile()
{
  if (pidfile) {
    unlink(pidfile);
}
}

/* Signal handler called when we receive the TERM signal */
void handle_term_signal(int signum)
{
  terminated=1;
}

/* Signal handler called when we receive the HUP signal */
void handle_hup_signal(int signum)
{
  restart_requested=1;
}

/* This is the watcher thread, that spawns the actual executable */
void do_watcher()
{
  struct sigaction new_action;
  pid_t child;
  char * oldcwd;

  // First we must detach and set up our io
  close(0);
  close(1);
  close(2);
  open("/dev/null", 0); // stdin
  if (overwrite_log) {
    if (0>open(logfile, O_WRONLY|O_TRUNC|O_CREAT, S_IRUSR|S_IWUSR)) {  // stdout
      exit(1); // We can't tell the user..
}
} else {
    if (0>open(logfile, O_WRONLY|O_APPEND|O_CREAT, S_IRUSR|S_IWUSR)) {  // stdout
      exit(1); // We can't tell the user..
}
}
  dup(1); // stderr
  setpgrp(); // Leave process group
  
  // Set up signal handling
  new_action.sa_handler = handle_term_signal;
  sigemptyset (&new_action.sa_mask);
  new_action.sa_flags = 0;
  
  new_action.sa_handler = handle_hup_signal;
  sigaction (SIGHUP, &new_action, NULL);

  new_action.sa_handler = handle_term_signal;
  sigaction (SIGINT, &new_action, NULL);
  sigaction (SIGTERM, &new_action, NULL);

  if (cwd) {
    oldcwd=getcwd(NULL, 0);
    if (chdir(cwd) <0) {
      fprintf(stderr, "[WATCHER] FATAL: Couldn't chdir to %s: %m\n", cwd);
      exit(1);
}
}

  fprintf(stderr, "[WATCHER] Watcher initialised\n");
  do {
    // Fork, and start the child.
    child=fork();
    if (child==0) {
      fprintf(stderr, "[WATCHER] Starting %s\n", exe_vptr[0]);
      if (quiet_log) {
        // Re-nuke stdout/stderr
        close(1);
        close(2);
        dup(0); // /dev/null
        dup(0);
}
      execv(exe_vptr[0], exe_vptr);
      fprintf(stderr, "[WATCHER] Exec failed: %m\n");
      exit(1);
} else if (child <0) {
      fprintf(stderr, "[WATCHER] Fork failed: %m\n");
} else {
      int child_dead=0;
      do {
        int status;
        // Wait for child to complete (or signal to arrive)
        pid_t res=wait(&status);
        if (res==child) {
          child_dead=1;
          fprintf(stderr, "[WATCHER] Child died, exit status=%d.\n", WEXITSTATUS(status));
} else if (restart_requested) {
          fprintf(stderr, "[WATCHER] Restart requested, sending SIGTERM to process %d.\n", child);
          kill(child,SIGTERM);
          continue;
} else if (terminated) {
          fprintf(stderr, "[WATCHER] Exit requested, sending SIGTERM to process %d.\n", child);
          kill(child,SIGTERM);
          continue;
}
} while(!child_dead);
}
    if (restart_requested==0 && terminated==0) {
      // Process died, so give a short delay before we respawn
      fprintf(stderr, "[WATCHER] Sleeping %d seconds before respawning.\n", sleeptime);
      sleep(sleeptime);
}
} while(terminated==0);
  fprintf(stderr, "[WATCHER] Exiting by request.\n");
  if (cwd && oldcwd)
{
      chdir(oldcwd);
}
  rmpidfile();
  exit(0);
}

void do_spawn()
{
  pid_t pid = fork();
  if (pid<0) {
    fprintf(stderr, "Error: Fork failed: %m\n");
    exit(1);
}

  if (pid==0) {
    do_watcher();
    exit(0);
} else {
    // This is the parent
    writepidfile(pid);
    fprintf(stdout, "[%d]\n", pid);
    exit(0);
} 
}

// Parse options and run mode. Return index of first unconsumed argument.
int parse_arguments(int argc, char *argv[])
{
  while (1) 
{
      static struct option long_options[] =
{
        /* These options set a flag. */
        //{"verbose", d_argument,       &verbose_flag, 1},
        //{"pidfile",   no_argument,       &verbose_flag, 0},
        /* These options don't set a flag.
           We distinguish them by their indices. */
{"help",     no_argument,       0, 'h'},
{"logfile",  required_argument, 0, 'l'},
{"pidfile",  required_argument, 0, 'p'},
{"chdir",    required_argument, 0, 'd'},
{"delay",    required_argument, 0, 't'},
{"truncate-log", no_argument,   0, 'o'},
{"quietlog",   no_argument,     0, 'q'},
{0, 0, 0, 0}
};
      /* `getopt_long' stores the option index here. */
      int option_index = 0;

      // opterr = 0; // DJS: sometimes this picks up your program's long args..bad!
      // setting opterr to zero will cause it to ignore unknown args and pass these on
      // but I haven't got it fully worked out yet. 
      int c = getopt_long (argc, argv, "hl:p:d:t:oq",
                       long_options, &option_index);
      /* Detect the end of the options. */
      if (c == -1)
        break;
      
      switch (c)
{
    case 'h':
          show_help(argv[0]);
          exit(0);
          break;

    case 'l':
          logfile=optarg;
          break;
          
    case 'p':
          pidfile=optarg;
          break;
          
    case 'd':
          cwd=optarg;
          break;
          
    case 't':
          sleeptime=atoi(optarg);
          break;
          
    case 'o':
          overwrite_log=1;
          break;
          
    case 'q':
          quiet_log=1;
          break;
          
    case '?':
          /* `getopt_long' already printed an error message. */
          break;
          
    default:
          fprintf(stderr, "Unexpected error\n");
          exit(1);
}
}
  
  /* Print any remaining command line arguments (not options). */
  if (optind < argc)
{
      if (!strcmp(argv[optind], "start")) {
        runmode=Start;
} else if (!strcmp(argv[optind], "stop")) {
        runmode=Stop;
} else if (!strcmp(argv[optind], "restart")) {
        runmode=Restart;
}
}
  if (runmode==Undef)
{
      fprintf(stderr, "You must specify one of start, stop or restart\n");
      exit(1);
}
  optind++;
  if (runmode==Start) {
    if (optind==argc) {
      fprintf(stderr, "You must specify the executable to start.\n");
      exit(1);
}
    exe_vptr=argv+optind;
    if (!logfile) {
      logfile="/dev/null";
}
}
  if (!pidfile && ((runmode==Stop) || (runmode==Restart))) {
    fprintf(stderr, "You must supply a pidfile for restart and stop\n");
    exit(1);
}
  return optind;
}

int main(int argc, char *argv[])
{
  pid_t pid;

  parse_arguments(argc, argv);

  if (runmode==Start) {
    // Check if we are already running
    pid=getpidfilepid();
    if (pid != 0) {
      if ( (kill(pid, 0) == 0) || errno != ESRCH) {
        fprintf(stderr, "Fatal: Already running, pid=%d\n", pid);
        exit(1);
}
      fprintf(stderr, "Warning: Stale pid file found (pid=%d), ignored\n",pid);
}
    do_spawn();
} else if (runmode==Stop) {
    pid=getpidfilepid();
    if (pid==0) {
      fprintf(stderr, "Error: PID file not found (or corrupt)\n");
      exit(1);
}
    if (kill(pid,0) != 0) {
      if (errno=ESRCH) {
        fprintf(stderr, "Warning: PID %d is not running - stale pidfile removed\n", pid);
        rmpidfile();
        exit(0);
} else {
        fprintf(stderr, "Error: Can't access PID %d: %m\n", pid);
        exit(1);
}
}
    kill(pid,SIGTERM);
} else if (runmode==Restart) {
    pid=getpidfilepid();
    if (pid==0) {
      fprintf(stderr, "Error: PID file not found (or corrupt)\n");
      exit(1);
}
    if (kill(pid,0) != 0) {
      if (errno=ESRCH) {
        fprintf(stderr, "Warning: PID %d is not running - stale pidfile removed\n", pid);
        rmpidfile();
        exit(0);
} else {
        fprintf(stderr, "Error: Can't access PID %d: %m\n", pid);
        exit(1);
}
}
    kill(pid,SIGHUP);
} else {
    fprintf(stderr,"Undefined runmode??");
    exit(1);
}
  exit(0);
}
