# Smallsh (Small Shell)
A custom interactive shell implementing a subset of well-known shell features. Written in C.

## Features
- Single-line shell commands
- Built-in `exit`, `cd`, and `status` commands
- Execution of other PATH binaries
- I/O redirection
- Foreground and background processes
- Custom handlers for `SIGINT` and `SIGTSTP` signals

## Limitations
- Maximum of 2048 characters in command, including variable expansion of `$$`
- Maximum of 512 arguments, including the command name
- Maximum of 500 concurrent background processes

## Compilation
- Compile using GCC with the `-std=gnu99` flag for C99 standards with GNU extensions
- Full command: `gcc -O3 -std=gnu99 -Wall -Wextra -Wpedantic -Werror smallsh.c -o smallsh`
- Alternatively, run `make` using the provided Makefile

## Usage
- Start the shell by running `./smallsh`
- Once started, the shell will display `:` as its command prompt
- Smallsh supports commands in the following format:
  - `command [arg1 arg2 ...] [< input_file] [> output_file] [&]`
  - Where items in square brackets are optional
    - `command` is the name of the command or PATH binary you wish to execute
    - `<` will redirect input from `stdin` to the specified `input_file`
    - `>` will redirect output from `stdout` to the specified `output_file`
    - `&` is a flag to run the process in the background
- Run `cd [path]` to change the working directory. Supports both relative and absolute paths.
- Run `status` to display the exit value or termination signal of the last terminated process
- Run `exit` to kill any background processes and terminate the shell

## Example
```bash
$ ./smallsh
: ls
junk  smallsh  smallsh.c
: ls > junk
: status
exit value 0
: cat junk
junk
smallsh
smallsh.c
: wc < junk > junk2
: wc < junk
 3  3 23
: test -f badfile
: status
exit value 1
: wc < badfile
cannot open badfile for input
: status
exit value 1
: badfile
badfile: No such file or directory
: sleep 5
^Cterminated by signal 2
: status &
terminated by signal 2
: sleep 15 &
background pid is 4338
: ps
  PID TTY          TIME CMD
 4236 pts/0    00:00:00 bash
 4328 pts/0    00:00:00 smallsh
 4338 pts/0    00:00:00 sleep
 4339 pts/0    00:00:00 ps
:
: # that was a blank command line, this is a comment
:
background pid 4338 is done: exit value 0
: # the background sleep finally finished
: sleep 30 &
background pid is 4340
: kill -15 4340
background pid 4340 is done: terminated by signal 15
: pwd
/home/rngolam/smallsh
: cd
: pwd
/home/rngolam
: echo 4328
4328
: echo $$
4328
: ^C^Z
Entering foreground-only mode (& is now ignored)
: date
Sat May  7 21:11:11 PDT 2022
: sleep 5 &
: date
Sat May  7 21:11:16 PDT 2022
: ^Z
Exiting foreground-only mode
: date
Sat May  7 21:12:33 PDT 2022
: sleep 5 &
background pid is 4350
: date
Sat May  7 21:12:33 PDT 2022
: exit
$ 
```
