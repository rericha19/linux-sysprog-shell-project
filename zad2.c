#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ARGS_ARR_SIZE 		512
#define HOST_STRING_LEN		256
#define INPUT_STRING_LEN 	256
#define SOCKET_PATH_LEN		256
#define max(a, b)			((a) > (b)) ? (a) : (b)

// controls whether some prints happen or not
// enabled by -v
int DEBUG_MODE = 0;
int temp_fd;
int client_s = 0;

// function to get current username, used by prompt printing func
const char *getusername() {
	uid_t uid = geteuid();
	struct passwd *pw = getpwuid(uid);
	if (pw != NULL)
		return pw->pw_name;
	return "";
}


// function to get current time, used by prompt printing func
const char *getcurrtime() {
	static char timestring[8];
	
	// the syscall expects an unsigned int, time_t didnt work
	// asm used
	unsigned int sec_temp;
	asm("call time; mov %%eax, %0;"		// call sys func time via asm
		: "=r" (sec_temp)				// return value
	);		
		
	// formatting to hh:mm format
	time_t sec = sec_temp;
	struct tm *info = localtime(&sec);	
	strftime(timestring, sizeof(timestring), "%H:%M ", info);
	return timestring;
}


// prints the "HH:MM user@machine#" prompt
void print_prompt() {		
	static char hoststring[HOST_STRING_LEN];
	gethostname(hoststring, HOST_STRING_LEN); 
	
	// changes font to red & bold
	printf("\033[1;31m");	
	
	printf("%s", getcurrtime());		
	printf("%s@", getusername());
	printf("%s# ", hoststring);
	
	// resets color
	printf("\033[0m");		
}


// help print with author name and usage
void print_help() {

	int saved_out = dup(STDOUT_FILENO);

	if (temp_fd != 0) {
		close(STDOUT_FILENO);
		dup(temp_fd);
		// close(temp_fd);
	}
	
	printf("\n");
	printf("\t Author: Samuel Řeřicha\n");
	printf("\t Tool:   custom shell tool with redirection and pipes\n");
	printf("\t         can be used in client-server form\n");
	printf("\t Usage:  <name> -c -s -p <port> -v -h\n");
	printf("\t         -h:  prints help\n");
	printf("\t         -s:  launch as server\n");
	printf("\t         -c:  launch as client\n");
	printf("\t         -v:  print debug strings\n");
	printf("\t         -p <port>: port to communicate on\n\n");

	
	if (temp_fd != 0) {
		dup2(saved_out, 0);
		close(saved_out);
		close(temp_fd);
	}
}


// used to split input by special characters
static void split_args(char *help_ptr, char **args_master, int iter, char **input) {
	*(help_ptr + 1) = 0;
	*(help_ptr - 1) = 0;
	args_master[iter + 1] = help_ptr;
	args_master[iter + 2] = help_ptr + (char) 2;
	*input = help_ptr + (char) 2;

}


// prints args in the form they are split
void debug_arg_print(char ***args2) {
	for (int i = 0; i < ARGS_ARR_SIZE; i++) {
		int has_something = 0;
		fprintf(stderr, "%d\n", i);
		for (int j = 0; j < ARGS_ARR_SIZE; j++) {
			if (args2[i][j] == NULL)
				break;
			has_something = 1;
			fprintf(stderr, "\t %d %s\n", j, args2[i][j]);
		}
		if (!has_something)
			break;
	}
}


// deallocates 3d args array's dynamically allocated memory
void free_args(char ***args) {
	for (int i = 0; i < ARGS_ARR_SIZE; i++)
		if (args[i] != NULL)
			free(args[i]);
		else break;
		
	free(args);
}


// input handling
// does # and \ handling, splits
// into 3d array by <>|; chars and by spaces
char*** manage_input(char *input) {
	
	while (input[0] == ' ')
		input++;
	
	char temp[5] = "";
	strncpy(temp, input, 4);
	
	if (strcmp(temp, "halt") == 0) {
		if (DEBUG_MODE)
			fprintf(stderr, "exitting cuz found halt\n");
			
		if (client_s != 0) {
			write(client_s, "quit", 5);
			close(client_s);
		}
		exit(0); 		
	}
		
	if (strcmp(temp, "quit") == 0) {
		if (DEBUG_MODE)
			fprintf(stderr, "exitting cuz found quit\n");
		if (client_s != 0) {
			write(client_s, "quit", 5);
			close(client_s);
		}
		exit(0);
	}	
	
	if (strcmp("help", temp) == 0) {
		print_help();
		return NULL; 
	}	

	// takes care of the # special character
	for (int i = 1; i < strlen(input); i++) {
		if (input[i] == '"' || input[i] == '`' || input[i] == '$' || input[i] == '\\' ) {
			if (input[i - 1] == '\\')
				memmove(input + i - 1,  input + i, strlen(input + i) + 1);			
			else 
				memmove(input + i,  input + i + 1, strlen(input + i) + 1);
		}			
	}
		
	// takes care of the '#' special character
	char *help_ptr;
	help_ptr = strchr(input, '#');
	if (help_ptr != NULL)
		*help_ptr = 0;
	
	// removes newline chars
	while (1) {
		help_ptr = strchr(input, '\n');
		if (help_ptr == NULL)
			break;
		else 
			*help_ptr = ' ';
	}	
	while (1) {
		help_ptr = strchr(input, '\r');
		if (help_ptr == NULL)
			break;
		else 
			*help_ptr = ' ';
	}
	
	// splits by |<>;
	int iter_master = 0;	
	char *args1[ARGS_ARR_SIZE];
	args1[0] = input;
	
	for (iter_master = 0; ; iter_master += 2) {
		help_ptr = NULL; 
		
		for (int iter = 0; ; iter++) {
			char curr_char = input[iter];
			if (curr_char == 0)
				break;
			if (strchr("|<>;", curr_char)) {
				help_ptr = (char *) (input + iter);
				break;
			}
		}
		
		if (help_ptr != NULL) 
			split_args(help_ptr, args1, iter_master, &input); 
		else 
			break;					
	} 
	
	// splits rows by spaces
	char ***args2 = (char ***) malloc(ARGS_ARR_SIZE * sizeof(char **));	
	for (int i = 0; i < ARGS_ARR_SIZE; i++) {
		args2[i] = (char **) malloc(ARGS_ARR_SIZE * sizeof(char *));
		for (int j = 0; j < ARGS_ARR_SIZE; j++)
			args2[i][j] = NULL;
	}
			
	for (int i = 0; i < iter_master + 1; i++) {
		if (DEBUG_MODE)
			fprintf(stderr, "%s\n", args1[i]);
		if (i % 2 == 0) {
			char *ptr = args1[i];
			int did_thing = 0;
			for (int j = 0; ; j++) {
				char *arg = strtok_r(ptr, " ", &ptr);
				if (arg == NULL)
					break;
				did_thing = 1;
				args2[i][j] = arg;
			}
			if (!did_thing)
				args2[i][0] = args1[i];			
		} else {
			args2[i][0] = args1[i];
		}
	}	
	
	return args2;
}


// counts commands in the commands array
// includes "rows" with <>|\;
int count_commands(char ***commands) {
	int count = 0;
	for (int i = 0; i < ARGS_ARR_SIZE; i++) {
		int has_something = 0;
		for (int j = 0; j < ARGS_ARR_SIZE; j++) {
			if (commands[i][j] == NULL)
				break;
			has_something = 1;
			break;
		}
		if (!has_something)
			break;
		count = i + 1;
	}
	return count;
}


// executes child command with specified in and out redirections
void launch_wrapper(char **args, int *output, int *input) {

	int saved_out = dup(STDOUT_FILENO);
	int saved_in = dup(STDIN_FILENO);
		
	// output redirection (>)
	if (output != NULL) {
		close(STDOUT_FILENO);
		dup(*output);
		//close(*output);
		
		//dup2(*output, STDOUT_FILENO);
		//close(*output);
	}
	
	if (temp_fd != 0 && output == NULL) {
		close(STDOUT_FILENO);
		dup(temp_fd);
		// close(temp_fd);
	}

	// input redirection (<)
	if (input != NULL) {
		//close(STDIN_FILENO);
		//dup(*input);
		//close(*input);
		
		close(STDIN_FILENO);
		dup2(*input, STDIN_FILENO);
		// close(*input);
	}

	// child	
	pid_t pid = fork();
	if (pid == 0) {			
		if (execvp(args[0], args) == -1) {
			perror("execvp failed");
			exit(EXIT_FAILURE);
		}
	}
	else if (pid < 0) {
		perror("fork failed");
		
	} else {
		// parent
		int status;
		do {
			waitpid(-1, &status, WUNTRACED);
		} while(!WIFEXITED(status) && !WIFSIGNALED(status));
	}	
	
	// restore stdout after redirection
	if (output != NULL) {
		dup2(saved_out, STDOUT_FILENO);
		close(saved_out);
		close(*output);
	}	
	
	if (temp_fd != 0 && output == NULL) {
		dup2(saved_out, 0);
		// close(saved_out);
		// close(temp_fd);
	}
	
	// restore stdin after redirection
	if (input != NULL) {
		dup2(saved_in, STDIN_FILENO);
		close(saved_in);
		close(*input);
	}
}


// manages file descriptors, redirection and executing the child project
void launch_wrapper_pipe(int fd, int *fds, char **args)
{
    if (dup2(fds[fd], fd) == -1) {
        perror("dup2");
        exit(1);
    }

    if (close(fds[0]) == -1) {
        perror("close");
        exit(1);
    }
    if (close(fds[1]) == -1) {
        perror("close");
        exit(1);
    }

    if (execvp(args[0], args) == -1) {
		perror("execvp failed");
		exit(EXIT_FAILURE);
	}
}


// function that handles pipes
// only works for cmd1 | cmd2 cases
int pipe_handle(char **args1, char **args2)
{
    int fds[2];
    pid_t pid1, pid2;
    
    int saved_out = dup(STDOUT_FILENO);

	if (temp_fd != 0) {
		if (DEBUG_MODE)
			fprintf(stderr, "im in help and tempfd thing in pipe hand\n");
		close(STDOUT_FILENO);
		dup(temp_fd);
		// close(temp_fd);
	}		
    

	// create pipe
    if (pipe(fds) == -1) {
        perror("pipe");
        exit(1);
    }

	// fork child1
    pid1 = fork();
    if (pid1 == -1) {
        perror("fork");
        exit(1);
    }
	if (pid1 == 0) 
        launch_wrapper_pipe(1, fds, args1);  

	// fork child 2
    pid2 = fork();
    if (pid2 == -1) {
        perror("fork");
        exit(1);
    }
    if (pid2 == 0) 
        launch_wrapper_pipe(0, fds, args2);   
    
	// close redirections
	if (close(fds[0]) == -1) {
        perror("close");
        exit(1);
    }
    if (close(fds[1]) == -1) {
        perror("close");
        exit(1);
    }

	// parent waits for children to finish
    pid_t w_pid;
    int status;
    while ((w_pid = wait(&status)) > 0);

    if (w_pid == -1 && errno != ECHILD) {
        perror("wait");
        exit(1);
    }
        
	if (temp_fd != 0) {
		dup2(saved_out, 0);
		close(saved_out);
		close(temp_fd);
	}

    return 0;
}


// goes thru the 3d command array and executes its individual commands 
void launch_commands(char ***commands) {
	int com_count = count_commands(commands);	
	int curr_command = 0;
	int* out = NULL;
	int* in = NULL;
	int fd1, fd2;
	if (DEBUG_MODE)
		fprintf(stderr, "com count %d\n", com_count);	
		
	for (int i = 0; i < com_count; i += 2) 
	{				
		// if next or last command is found, execute current one
		if (((i + 1) == com_count)
			 || ((i + 1) < com_count && commands[i + 1][0][0] == ';')) {
			launch_wrapper(commands[curr_command], out, in);
			out = NULL;			
			in = NULL;
			curr_command = i + 2;
		}
		// if > is found, save file descriptor for it
		else if ((i + 1) < com_count && commands[i + 1][0][0] == '>') {
			remove(commands[i + 2][0]);
			fd1 = open(commands[i + 2][0], O_WRONLY | O_CREAT, 0777);
			if (fd1 != -1)
				out = &fd1;			
			else {
				printf("file %s could not be opened, aborting\n", commands[i + 2][0]);
				return;
			}
		}
		// if < is found, save file descriptor for it
		else if ((i + 1) < com_count && commands[i + 1][0][0] == '<') {
			fd2 = open(commands[i + 2][0], O_RDONLY);
			if (fd2 != -1)
				in = &fd2;
			else {
				printf("file %s could not be opened, aborting\n", commands[i + 2][0]);
				return;
			}
		}
		// if pipe is found, assume cmd1 | cmd2 and execute
		else if ((i + 1) < com_count && commands[i + 1][0][0] == '|') {
			pipe_handle(commands[i], commands[i + 2]);
			i += 2;
			curr_command = i + 4;
		}
	}
	
}


// reads input line by line
char *read_input() {
	char buffer[INPUT_STRING_LEN] = {0};

	print_prompt();
	//scanf("%[^\n]s ", buffer); 
	fgets(buffer, INPUT_STRING_LEN, stdin);	
	// fgetc(stdin);
	char ***commands = manage_input(buffer);
	
	if (commands != NULL) {
		if (DEBUG_MODE)
			debug_arg_print(commands);
		launch_commands(commands);
		free_args(commands);
	}
}


void server_main(int port, char *sck_path) {
	if (DEBUG_MODE)
		fprintf(stderr, "im server, port %d\n", port);
	char client_mess[1024];
	
	int sckt = socket(AF_INET , SOCK_STREAM , 0);
	if (sckt == -1) 	{
		perror("socket failure");
		exit(1);
	}
	
	struct sockaddr_in server;
	server.sin_family = AF_INET;	
	server.sin_port = htons(port);
	server.sin_addr.s_addr = INADDR_ANY;
	printf("communicating at port %d\n", port);

	if(bind(sckt, (struct sockaddr *) &server, sizeof(server)) < 0) {
		perror("bind failed");
		exit(1);
	}
	
	listen(sckt , 3);

	int c = sizeof(struct sockaddr_in);
	struct sockaddr_in client;
	int client_sock;
	
	client_sock = accept(sckt, (struct sockaddr *) &client, &c);
	if (client_sock < 0) {
		perror("accept failed");
		exit(1);
	}		
	
	client_s = client_sock;		
	
	unsigned char buffer[65536];
	int read_size;
	while((read_size = recv(client_sock, client_mess , 2000 , 0)) > 0 ) {
		if (DEBUG_MODE)
			fprintf(stderr, "read size %d\n", read_size);
		
		temp_fd = 0;
		temp_fd = open("temp68682866", O_RDWR | O_CREAT, 0777);
		
		char ***commands = manage_input(client_mess);		
		
		if (commands != NULL) {
			if (DEBUG_MODE)
				debug_arg_print(commands);
			launch_commands(commands);						
			free_args(commands);
		}		
		
		// fprintf(stderr, "%d\n", close(temp_fd));
		
		close(temp_fd);
		FILE *temp = fopen("temp68682866", "r");
		fseek(temp, 0, SEEK_END);
		int fsize = ftell(temp);
		rewind(temp);
		memset(buffer, 0, 65536);
		fread(buffer, 1, 65536, temp);
					
		if (strlen(buffer))
			write(client_sock, buffer, strlen(buffer) + 1);
		else
			write(client_sock, " ", 2);
			
		remove("temp68682866");
	}
	
	
	if(read_size == 0) {
		fprintf(stderr, "client left\n");
		exit(0);
	}
		
	if(read_size == -1) {
		perror("recv failed");
		exit(1);
	}
}


void client_main(int port) {
	if (DEBUG_MODE)
		fprintf(stderr, "im client now\n");
	char message[1024], reply[1024];
	
	int sckt = socket(AF_INET, SOCK_STREAM , 0);
	if (sckt == -1) {
		perror("socket failed");
		exit(1);
	}
	
	struct sockaddr_in server;
	server.sin_addr.s_addr = inet_addr("127.0.0.1");
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	printf("communicating at port %d\n", port);

	int server_fd = connect(sckt, (struct sockaddr *) &server, sizeof(server));
	if (server_fd < 0)	{
		perror("connect failed");
		close(sckt);
		exit(1);
	}
	
	
	while(1) {
		printf("\r");
		print_prompt();
		fgets(message, sizeof(message), stdin);
		//scanf("%[^\n]s ", message); 
		if (strcmp("halt", message) == 0)
			exit(0);
			
		if(send(sckt, message, strlen(message) + 1, 0) < 0) {
			perror("send failed");
			close(sckt);
			exit(1);
		}
		
		if(recv(sckt, reply, sizeof(reply), 0) < 0) {
			perror("recv failed");
			close(sckt);
			exit(1);
		}
		
		if (strcmp("quit", reply) == 0)
			break;
		printf("%s", reply);
	}
	
	close(sckt);
	exit(0);
}


// main func, handles cmd line arguments and 
// switches to appropriate loop for the program
int main(int argc, char **argv, char **envp) {
	int arg;
	int is_client = 0;
	int is_server = 0;	
	int port = -1;
	char socket[SOCKET_PATH_LEN] = "";
	
	// built in C function for parsing command line args
	while ((arg = getopt(argc, argv, "schp:v")) != -1) {
		switch(arg) {
			case 's': 
				is_server = 1;
				break;
			case 'c':
				is_client = 1;
				break;			
			case 'p':
				port = atoi(optarg);
				// printf("port %d\n", port);
				break;
			case 'h':
				print_help();
				exit(0);
				break;
			case 'v':
				printf("debug mode set\n");
				DEBUG_MODE = 1;
				break;
			case ':':
				printf("option %c requires a value\n", optopt);
				break;
			case '?':
				printf("unknown option %c\n", optopt);
				break;
		}
	}
	
	if (port == -1)
		port = 1234;
	
	if (is_server)
		server_main(port, socket);
	if (is_client)
		client_main(port);
		
	printf("Neither client nor server were specified\n");
	temp_fd = 0;
	while (1)
		read_input();

	fprintf(stderr, "got here somehow\n");
	return 0;
}

