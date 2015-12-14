// -*- mode: c++ -*-
#define _BSD_EXTENSION
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <dirent.h>
#include <netdb.h>
#include <assert.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include "md5.h"
#include "osp2p.h"

#include <sys/types.h> // for pid_t
#include <sys/wait.h> // for waitpid

static struct in_addr listen_addr;	// Define listening endpoint
static int listen_port;


/*****************************************************************************
 * TASK STRUCTURE
 * Holds all information relevant for a peer or tracker connection, including
 * a bounded buffer that simplifies reading from and writing to peers.
 */

#define TASKBUFSIZ	65535
#define BLOCKSIZ 1024

 // Size of task_t::buf
#define FILENAMESIZ	256	// Size of task_t::filename
#define MAXFILESIZ 1024*1024 * 4 // 1 Max file size is 4MB 
#define MINRATE 256 //Minimum download rate for a task otherwise conection is dropped. bytes transfered per block
#define SAMPLESIZ 8 //Amount of samples used to check fsor download rate.

typedef enum tasktype {		// Which type of connection is this?
	TASK_TRACKER,		// => Tracker connection
	TASK_PEER_LISTEN,	// => Listens for upload requests
	TASK_UPLOAD,		// => Upload request (from peer to us)
	TASK_DOWNLOAD,		// => Download request (from us to peer)
	TASK_DOWNLOAD_MULT_PEERS,
	TASK_UPLOAD_MULT_PEERS
} tasktype_t;

typedef struct peer {		// A peer connection (TASK_DOWNLOAD)
	char alias[TASKBUFSIZ];	// => Peer's alias
	struct in_addr addr;	// => Peer's IP address
	int port;		// => Peer's port number
	
	//linked list members
	struct peer *next;
	struct peer *prev;
	
	int file_fd;
	int transfer_rate;
	int fail_rate;
	
	char buf[TASKBUFSIZ];
	unsigned head;
	unsigned tail;
	
	int num_uploading_peers;
	int block_offset; // This is essentially the peers id;
} peer_t;

typedef struct task {
	tasktype_t type;	// Type of connection
	
	//for segmented upload
	char flag;
	int block_offset;
	int num_uploading_peers;

	int peer_fd;		// File descriptor to peer/tracker, or -1
	int disk_fd;		// File descriptor to local file, or -1

	char buf[TASKBUFSIZ];	// Bounded buffer abstraction
	unsigned head;
	unsigned tail;
	size_t total_written;	// Total number of bytes written
				// by write_to_taskbuf

	char filename[FILENAMESIZ];	// Requested filename
	char disk_filename[FILENAMESIZ]; // Local filename (TASK_DOWNLOAD)

	peer_t *peer_list;	// List of peers that have 'filename'
				// (TASK_DOWNLOAD).  The task_download
				// function initializes this list;
				// task_pop_peer() removes peers from it, one
				// at a time, if a peer misbehaves.
				
	peer_t *multi_peer_list;
	
	
	int *peer_count;
	int *multi_peer_count;
	int in_progress_transfers;
} task_t;


// task_new(type)
//	Create and return a new task of type 'type'.
//	If memory runs out, returns NULL.
static task_t *task_new(tasktype_t type)
{
	task_t *t = (task_t *) malloc(sizeof(task_t));
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}

	t->type = type;
	t->peer_fd = t->disk_fd = -1;
	t->head = t->tail = 0;
	t->total_written = 0;
	t->peer_list = NULL;
	
	t->multi_peer_count = (int *) malloc(sizeof (int));
	t->peer_count = (int *) malloc(sizeof (int));
	
	*t->multi_peer_count = 0;
	*t->peer_count = 0;

	t->in_progress_transfers = 0;

	strcpy(t->filename, "");
	strcpy(t->disk_filename, "");

	return t;
}

// task_pop_peer(t)
//	Clears the 't' task's file descriptors and bounded buffer.
//	Also removes and frees the front peer description for the task.
//	The next call will refer to the next peer in line, if there is one.
static void task_pop_peer(task_t *t, peer_t *p, peer_t *peer_list, int *count)
{
	if (t) {
		//we don't modify the buffers of the peer or its file descriptors

		//if peer_list valid
		if (peer_list) {
			//if peer to remove is specified:
			if (p) {
				if (p->prev == NULL && p->next == NULL) {
					//do nothing
				} else if (p->prev == NULL) {
					p->next->prev = NULL;
				} else if (p->next == NULL) {
					p->prev->next = NULL;
				} else {
					p->next->prev = p->prev;
					p->prev->next = p->next;
				}
				
				if (p == peer_list) {
					t->peer_list = p->next;
				}
				*count--;
			}
		}
		//we don't dispose of peer members in this function
	}
}

//stronger case for popping peer - here we reset buffers and dispose of peer
static void task_pop_top_peer(task_t *t)
{
	if (t) {
		// Close the file descriptors and bounded buffer
		if (t->peer_fd >= 0)
			close(t->peer_fd);
		if (t->disk_fd >= 0)
			close(t->disk_fd);
		t->peer_fd = t->disk_fd = -1;
		t->head = t->tail = 0;
		t->total_written = 0;
		t->disk_filename[0] = '\0';

		//if t has a peer list
		if (t->peer_list) {
			peer_t *n = t->peer_list->next;
			free(t->peer_list);
			t->peer_list = n;
			t->peer_count--;
		}
	}
}

//should have also a peer destructor to manage our resources but no time/not essential

// task_free(t)
//	Frees all memory and closes all file descriptors relative to 't'.
static void task_free(task_t *t)
{
	//we should probably handle our additional members and file descriptors here
	if (t) {
		do {
			task_pop_top_peer(t);
		} while (t->peer_list);
		free(t);
	}
}


/******************************************************************************
 * TASK BUFFER
 * A bounded buffer for storing network data on its way into or out of
 * the application layer.
 */

typedef enum taskbufresult {		// Status of a read or write attempt.
	TBUF_ERROR = -1,		// => Error; close the connection.
	TBUF_END = 0,			// => End of file, or buffer is full.
	TBUF_OK = 1,			// => Successfully read data.
	TBUF_AGAIN = 2			// => Did not read data this time.  The
					//    caller should wait.
} taskbufresult_t;

// read_to_taskbuf(fd, t)
//	Reads data from 'fd' into 't->buf', t's bounded buffer, either until
//	't's bounded buffer fills up, or no more data from 't' is available,
//	whichever comes first.  Return values are TBUF_ constants, above;
//	generally a return value of TBUF_AGAIN means 'try again later'.
//	The task buffer is capped at TASKBUFSIZ.
taskbufresult_t read_to_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail || headpos < tailpos)
		amt = read(fd, &t->buf[tailpos], TASKBUFSIZ - tailpos);
	else
		amt = read(fd, &t->buf[tailpos], headpos - tailpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->tail += amt;
		return TBUF_OK;
	}
}


// write_from_taskbuf(fd, t)
//	Writes data from 't' into 't->fd' into 't->buf', using similar
//	techniques and identical return values as read_to_taskbuf.
taskbufresult_t write_from_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = write(fd, &t->buf[headpos], tailpos - headpos);
	else
		amt = write(fd, &t->buf[headpos], TASKBUFSIZ - headpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else 
	{
		t->head += amt;
		t->total_written += amt;
		return TBUF_OK;
	}
}

//read_from_taskbuf using positional pwrite
///////////////////////////
taskbufresult_t read_to_taskbuf2(int fd, task_t *t)
{
	unsigned headpos = (t->head % BLOCKSIZ);
	unsigned tailpos = (t->tail % BLOCKSIZ);
	ssize_t amt;

	if (t->head == t->tail || headpos < tailpos)
		amt = pread(fd, &t->buf[tailpos], BLOCKSIZ - tailpos, t->block_offset);
	else
		amt = pread(fd, &t->buf[tailpos], headpos - tailpos, t->block_offset);
	
	if (amt == BLOCKSIZ - tailpos || amt == headpos - tailpos)
		t->block_offset += *t->multi_peer_count * BLOCKSIZ;

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->tail += amt;
		return TBUF_OK;
	}
}




/////////////////

taskbufresult_t read_to_pbuf(peer_t *p, task_t *t)
{
	int fd = p->file_fd;
	unsigned headpos = (p->head % BLOCKSIZ);
	unsigned tailpos = (p->tail % BLOCKSIZ);
	ssize_t amt;
	

	if (t->head == t->tail || headpos < tailpos)
		amt = pread(fd, &p->buf[tailpos], BLOCKSIZ - tailpos, p->block_offset);
	else
		amt = pread(fd, &p->buf[tailpos], headpos - tailpos, p->block_offset);
	
	if (amt == BLOCKSIZ - tailpos || amt == headpos - tailpos)
		p->block_offset += *t->multi_peer_count * BLOCKSIZ;

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		p->tail += amt;
		t->tail += amt;
		return TBUF_OK;
	}
}

//read_from_taskbuf using positional pwrite
taskbufresult_t write_from_pbuf(peer_t *p, task_t *t)
{
	unsigned headpos = (p->head % BLOCKSIZ);
	unsigned tailpos = (p->tail % BLOCKSIZ);
	ssize_t amt;
	int fd = t->disk_fd;
	
	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = pwrite(fd, &p->buf[headpos], tailpos - headpos, p->block_offset);
	else
		amt = pwrite(fd, &p->buf[headpos], BLOCKSIZ - headpos, p->block_offset);
	
	if (amt == BLOCKSIZ - tailpos || amt == headpos - tailpos)
		p->block_offset += *t->multi_peer_count * BLOCKSIZ;

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else 
	{
		p->head += amt;
		t->head += amt;
		t->total_written += amt;
		return TBUF_OK;
	}
}



//write_from_taskbuf using positional pwrite
taskbufresult_t write_from_taskbuf2(int fd, task_t *t)
{
	unsigned headpos = (t->head % BLOCKSIZ);
	unsigned tailpos = (t->tail % BLOCKSIZ);
	ssize_t amt;
	
	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = pwrite(fd, &t->buf[headpos], tailpos - headpos, t->block_offset);
	else
		amt = pwrite(fd, &t->buf[headpos], BLOCKSIZ - headpos, t->block_offset);
	
	if (amt == BLOCKSIZ - tailpos || amt == headpos - tailpos)
		t->block_offset += *t->multi_peer_count * BLOCKSIZ;

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else 
	{
		t->head += amt;
		t->total_written += amt;
		return TBUF_OK;
	}
}



/******************************************************************************
 * NETWORKING FUNCTIONS
 */

// open_socket(addr, port)
//	All the code to open a network connection to address 'addr'
//	and port 'port' (or a listening socket on port 'port').
int open_socket(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	int fd, ret, yes = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
	    || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		goto error;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr = addr;
	saddr.sin_port = htons(port);

	if (addr.s_addr == INADDR_ANY) {
		if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1
		    || listen(fd, 4) == -1)
			goto error;
	} else {
		if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1)
			goto error;
	}

	return fd;

    error:
	if (fd >= 0)
		close(fd);
	return -1;
}


/******************************************************************************
 * THE OSP2P PROTOCOL
 * These functions manage connections to the tracker and connections to other
 * peers.  They generally use and return 'task_t' objects, which are defined
 * at the top of this file.
 */

// read_tracker_response(t)
//	Reads an RPC response from the tracker using read_to_taskbuf().
//	An example RPC response is the following:
//
//      FILE README                             \ DATA PORTION
//      FILE osptracker.cc                      | Zero or more lines.
//      ...                                     |
//      FILE writescan.o                        /
//      200-This is a context line.             \ MESSAGE PORTION
//      200-This is another context line.       | Zero or more CONTEXT lines,
//      ...                                     | which start with "###-", and
//      200 Number of registered files: 12      / then a TERMINATOR line, which
//                                                starts with "### ".
//                                                The "###" is an error code:
//                                                200-299 indicate success,
//                                                other codes indicate error.
//
//	This function empties the task buffer, then reads into it until it
//	finds a terminator line.  It returns the number of characters in the
//	data portion.  It also terminates this client if the tracker's response
//	is formatted badly.  (This code trusts the tracker.)
static size_t read_tracker_response(task_t *t)
{
	char *s;
	size_t split_pos = (size_t) -1, pos = 0;
	t->head = t->tail = 0;

	while (1) {
		// Check for whether buffer is complete.
		for (; pos+3 < t->tail; pos++)
			if ((pos == 0 || t->buf[pos-1] == '\n')
			    && isdigit((unsigned char) t->buf[pos])
			    && isdigit((unsigned char) t->buf[pos+1])
			    && isdigit((unsigned char) t->buf[pos+2])) {
				if (split_pos == (size_t) -1)
					split_pos = pos;
				if (pos + 4 >= t->tail)
					break;
				if (isspace((unsigned char) t->buf[pos + 3])
				    && t->buf[t->tail - 1] == '\n') {
					t->buf[t->tail] = '\0';
					return split_pos;
				}
			}

		// If not, read more data.  Note that the read will not block
		// unless NO data is available.
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR)
			die("tracker read error");
		else if (ret == TBUF_END)
			die("tracker connection closed prematurely!\n");
	}
}


// start_tracker(addr, port)
//	Opens a connection to the tracker at address 'addr' and port 'port'.
//	Quits if there's no tracker at that address and/or port.
//	Returns the task representing the tracker.
task_t *start_tracker(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	task_t *tracker_task = task_new(TASK_TRACKER);
	size_t messagepos;

	if ((tracker_task->peer_fd = open_socket(addr, port)) == -1)
		die("cannot connect to tracker");

	// Determine our local address as seen by the tracker.
	saddrlen = sizeof(saddr);
	if (getsockname(tracker_task->peer_fd,
			(struct sockaddr *) &saddr, &saddrlen) < 0)
		error("getsockname: %s\n", strerror(errno));
	else {
		assert(saddr.sin_family == AF_INET);
		listen_addr = saddr.sin_addr;
	}

	// Collect the tracker's greeting.
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's greeting:\n%s", &tracker_task->buf[messagepos]);

	return tracker_task;
}


// start_listen()
//	Opens a socket to listen for connections from other peers who want to
//	upload from us.  Returns the listening task.
task_t *start_listen(void)
{
	struct in_addr addr;
	task_t *t;
	int fd;
	addr.s_addr = INADDR_ANY;

	// Set up the socket to accept any connection.  The port here is
	// ephemeral (we can use any port number), so start at port
	// 11112 and increment until we can successfully open a port.
	for (listen_port = 11112; listen_port < 13000; listen_port++)
		if ((fd = open_socket(addr, listen_port)) != -1)
			goto bound;
		else if (errno != EADDRINUSE)
			die("cannot make listen socket");

	// If we get here, we tried about 200 ports without finding an
	// available port.  Give up.
	die("Tried ~200 ports without finding an open port, giving up.\n");

    bound:
	message("* Listening on port %d\n", listen_port);

	t = task_new(TASK_PEER_LISTEN);
	t->peer_fd = fd;
	return t;
}


// register_files(tracker_task, myalias)
//	Registers this peer with the tracker, using 'myalias' as this peer's
//	alias.  Also register all files in the current directory, allowing
//	other peers to upload those files from us.
static void register_files(task_t *tracker_task, const char *myalias)
{
	DIR *dir;
	struct dirent *ent;
	struct stat s;
	char buf[PATH_MAX];
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);


	// Register address with the tracker.
	osp2p_writef(tracker_task->peer_fd, "ADDR %s %I:%d\n",
		     myalias, listen_addr, listen_port);
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's response to our IP address registration:\n%s",
		&tracker_task->buf[messagepos]);
	if (tracker_task->buf[messagepos] != '2') {
		message("* The tracker reported an error, so I will not register files with it.\n");
		return;
	}

	// Register files with the tracker.
	message("* Registering our files with tracker\n");
	if ((dir = opendir(".")) == NULL)
		die("open directory: %s", strerror(errno));
	while ((ent = readdir(dir)) != NULL) {
		int namelen = strlen(ent->d_name);
		
		//Might want to limit the amount of files a peer can upload here

		// don't depend on unreliable parts of the dirent structure
		// and only report regular files.  Do not change these lines.
		if (stat(ent->d_name, &s) < 0 || !S_ISREG(s.st_mode)
		    || (namelen > 2 && ent->d_name[namelen - 2] == '.'
			&& (ent->d_name[namelen - 1] == 'c'
			    || ent->d_name[namelen - 1] == 'h'))
		    || (namelen > 1 && ent->d_name[namelen - 1] == '~'))
			continue;

		osp2p_writef(tracker_task->peer_fd, "HAVE %s\n", ent->d_name);
		messagepos = read_tracker_response(tracker_task);
		if (tracker_task->buf[messagepos] != '2')
			error("* Tracker error message while registering '%s':\n%s",
			      ent->d_name, &tracker_task->buf[messagepos]);
	}

	closedir(dir);
}


// parse_peer(s, len)
//	Parse a peer specification from the first 'len' characters of 's'.
//	A peer specification looks like "PEER [alias] [addr]:[port]".
static peer_t *parse_peer(const char *s, size_t len)
{
	peer_t *p = (peer_t *) malloc(sizeof(peer_t));
	if (p) {
		p->next = NULL;
		if (osp2p_snscanf(s, len, "PEER %s %I:%d",
				  p->alias, &p->addr, &p->port) >= 0
		    && p->port > 0 && p->port <= 65535)
			return p;
	}
	free(p);
	return NULL;
}


static void remove_and_advance(task_t* t, peer_t* current, peer_t* peer_list, int *count) {
	//if there is a peer after this one
	if (current->next) {
		//advance to it and remove the previous (formerly current) peer
		current = current->next;
		task_pop_peer(t, current->prev, peer_list, count);
	} else {
		//we are at end of peer list, so just pop off current peer and set current to NULL
		task_pop_peer(t, current, peer_list, count);
		current = NULL;
	}
}

//add peer into doubly linked peer list
static void add_peer(task_t* t, peer_t* p, peer_t* peer_list, int *count) {
	if (peer_list) {
		peer_list->prev = p;
		p->next = peer_list;
	}
	peer_list = p;
	if (t) {
		*count++;
	}
}

// start_download(tracker_task, filename)
//	Return a TASK_DOWNLOAD task for downloading 'filename' from peers.
//	Contacts the tracker for a list of peers that have 'filename',
//	and returns a task containing that peer list.
task_t *start_download(task_t *tracker_task, const char *filename)
{
	char *s1, *s2;
	task_t *t = NULL;
	peer_t *p;
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);
	int len = strlen(filename);
	
	//Exercise 2 Code
	if (len >= FILENAMESIZ)
	{
		error("Filename too long");
		goto exit;
	}

	int i;
	for (i = 1; i < len; i++)
	{
		if (filename[i] == '/')
		{
			error("Peer attempting to access outside working directory");
			goto exit;
		}
	}


	message("* Finding peers for '%s'\n", filename);

	osp2p_writef(tracker_task->peer_fd, "WANT %s\n", filename);
	messagepos = read_tracker_response(tracker_task);
	if (tracker_task->buf[messagepos] != '2') {
		error("* Tracker error message while requesting '%s':\n%s",
		      filename, &tracker_task->buf[messagepos]);
		goto exit;
	}

	if (!(t = task_new(TASK_DOWNLOAD))) {
		error("* Error while allocating task");
		goto exit;
	}
	
	//Changed this to strncpy instead of strcpy
	strncpy(t->filename, filename, len);

	// add peers
	s1 = tracker_task->buf;
	t->peer_count = 0;
	while ((s2 = memchr(s1, '\n', (tracker_task->buf + messagepos) - s1))) {
		if (!(p = parse_peer(s1, s2 - s1)))
			die("osptracker responded to WANT command with unexpected format!\n");
		/*p->next = t->peer_list;
		t->peer_list = p;*/
		add_peer(t, p, t->peer_list, t->peer_count);
		s1 = s2 + 1;
		//count++;
	}

	//t->peer_count = count;
	
	if (s1 != tracker_task->buf + messagepos)
		die("osptracker's response to WANT has unexpected format!\n");

 exit:
	return t;
}


// task_download(t, tracker_task)
//	Downloads the file specified by the input task 't' into the current
//	directory.  't' was created by start_download().
//	Starts with the first peer on 't's peer list, then tries all peers
//	until a download is successful.
static void task_download(task_t *t, task_t *tracker_task)
{
	int i, ret = -1, num_blocks = 0;
	double avg_rate;
	assert((!t || t->type == TASK_DOWNLOAD || t->type == TASK_DOWNLOAD_MULT_PEERS)
	       && tracker_task->type == TASK_TRACKER);

	// Quit if no peers
	if (!t || !t->peer_list) {
		error("* No peers are willing to serve '%s'\n",
		      (t ? t->filename : "that file"));
		task_free(t);
		return;
	} 
	
	error("DO WE GET HERE");
	// Open disk file for the result.
	// If the filename already exists, save the file in a name like
	// "foo.txt~1~".  However, if there are 50 local files, don't download
	// at all.
	for (i = 0; i < 50; i++) {
		if (i == 0)
			strcpy(t->disk_filename, t->filename);
		else
			sprintf(t->disk_filename, "%s~%d~", t->filename, i);
		t->disk_fd = open(t->disk_filename,
				  O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (t->disk_fd == -1 && errno != EEXIST) {
			error("* Cannot open local file");
			goto try_again;
		} else if (t->disk_fd != -1) {
			message("* Saving result to '%s'\n", t->disk_filename);
			break;
		}
	}
	if (t->disk_fd == -1) {
		error("* Too many local files like '%s' exist already.\n\
* Try 'rm %s.~*~' to remove them.\n", t->filename, t->filename);
		task_free(t);
		return;
	}
	
	/*else if (t->peer_list->addr.s_addr == listen_addr.s_addr
		   && t->peer_list->port == listen_port)
		goto try_again;*/


	//remove this peer from the peer list
	peer_t *current = t->peer_list;
	while (current != NULL) {
		if (current->addr.s_addr == listen_addr.s_addr
		   && current->port == listen_port) {
			   task_pop_peer(t, current, t->peer_list, t->peer_count);
		   }
	}
	
	// Connect to the peer and write the GET command
	// Code was modified in order to download from multiple peers at a time, instead of storing
	// The file descriptors in the task, the pertinent fd are stored on the peer
	current = t->peer_list;
	t->multi_peer_count = 0;
	while (current != NULL)
	{
		message("* Connecting to %s:%d to download '%s'\n",
		inet_ntoa(current->addr), current->port, t->filename);
		current->file_fd = open_socket(current->addr, current->port); //returns file descriptor to peers open socket
		
		//if connection not established
		if (current->file_fd == -1) {
			error("* Cannot connect to peer: %s\n", strerror(errno));
			//we remove this peer from our task altogther and move onto the next peer in line
			remove_and_advance(t, current, t->peer_list, t->peer_count);
		}
		
		current->block_offset = *t->multi_peer_count;
		
		//add current onto task list of special peers
		add_peer(t, current, t->multi_peer_list, t->multi_peer_count);
		
		//remove current from task list of regular peers
		remove_and_advance(t, current, t->peer_list, t->peer_count);
	}
	
	current = t->multi_peer_list;
	while (current != NULL) {
		//if connection established, we write our file request
		current->num_uploading_peers = *t->multi_peer_count;
		osp2p_writef(current->file_fd, "GET -%c %i %i %s OSP2P\n", 'm', current->num_uploading_peers, current->block_offset, t->filename);
	}
	t->in_progress_transfers = *t->multi_peer_count;

	
	//now we try a parallel download from multiple special peers, if we have any
	current = t->multi_peer_list;
	while (current != NULL) 
	{
		//this probably needs to be called with addiotnal and appropriate arguments
		int ret = read_to_pbuf(current, t);
		if (ret == TBUF_ERROR) {
			error("* Peer read error");
			goto not_special_peer;
		} else if (ret == TBUF_END && t->head == t->tail) {
			/* End of peer's file*/
			//remove peer from special peer list and advance to next peer, but do nothing else because successful transfer
			remove_and_advance(t, current, t->multi_peer_list, t->multi_peer_count);
			//if that was on the end of this list, move back to beginning of list before reentering while loop
			if (current == NULL) 
				current = t->multi_peer_list;
			t->in_progress_transfers--;
			continue;
		}
	
		
		//this should be modified to only write when there is a full block to write
		//should only write that full block
		if (current->tail % BLOCKSIZ == 0 && current->tail != 0)
			ret = write_from_pbuf(current, t);
		if (ret == TBUF_ERROR) {
			error("* Disk write error");
			goto not_special_peer;
		}

		//check amount this peer is downloading
		if (t->total_written >= MAXFILESIZ) {
			error("** File is too large \n");
			goto not_special_peer;
		}

		//check speed of connection to this peer
		num_blocks++;
		if (num_blocks > SAMPLESIZ) 
		{
			avg_rate = t->total_written / num_blocks;
			if (avg_rate < MINRATE) {
				error("* Peer connection is too slow, dropping connection\n");
				goto not_special_peer;
			}
	
		}
		
		//move on to next peer unless next peer is null, then move to beginning of multi peer list
		if (current->next) {
			current = current->next;
		} else {
			current = t->multi_peer_list;
		}
		continue;
		
		//remove peer from special peer list and submit another request for the file at this block offset
		//however, this could be a valid regular peer, so put back onto regular peer list
		//also increment successful peer completions
		not_special_peer:
		add_peer(t, current, t->peer_list, t->peer_count);
		peer_t* prev = current;
		remove_and_advance(t, current, t->multi_peer_list, t->multi_peer_count);
		if (current) {
			osp2p_writef(current->file_fd, "GET -%c %i %i %s OSP2P\n", 'm', prev->num_uploading_peers, prev->block_offset, t->filename);
		}
	}
	
	//if special peer list is empty and we have sucessfully transfered all parts of the file
	//check file recieved as normal
	if (!t->in_progress_transfers) {
		goto multi_peer_transfer_done;
	}
	
	//if special peer list is empty, but unable to complete file transfer, try regular type peers
	//connect to peer and write GET command
	current = t->peer_list;
	if (current != NULL)
	{
		message("* Connecting to %s:%d to download '%s'\n",
		inet_ntoa(current->addr), current->port, t->filename);
		t->peer_fd = open_socket(current->addr, current->port); //returns file descriptor to peers open socket
		if (t->peer_fd == -1) {
			error("* Cannot connect to peer: %s\n", strerror(errno));
			goto try_again;
		}
		osp2p_writef(t->peer_fd, "GET %s OSP2P\n", t->filename);
	}

	// Read the file into the task buffer from the peer,
	// and write it from the task buffer onto disk - serial type download
	while (1) 
	{
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer read error");
			goto try_again;
		} else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;
	
	
	
		ret = write_from_taskbuf(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk write error");
			goto try_again;
		}

		if (t->total_written >= MAXFILESIZ) {
			error("** File is too large \n");
			goto try_again;
		}

		num_blocks++;

		if (num_blocks > SAMPLESIZ) 
		{
			avg_rate = t->total_written / num_blocks;
			if (avg_rate < MINRATE) {
				error("* Peer connection is too slow, dropping connection\n");
				goto try_again;
			}
	
		}
		
	}
	
	multi_peer_transfer_done:
	// Empty files are usually a symptom of some error.
	if (t->total_written > 0) {
		message("* Downloaded '%s' was %lu bytes long\n",
			t->disk_filename, (unsigned long) t->total_written);
		// Inform the tracker that we now have the file,
		// and can serve it to others!  (But ignore tracker errors.)
		if (strcmp(t->filename, t->disk_filename) == 0) {
			osp2p_writef(tracker_task->peer_fd, "HAVE %s\n",
				     t->filename);
			(void) read_tracker_response(tracker_task);
		}
		task_free(t);
		return;
	}
	error("* Download was empty, trying next peer\n");

    try_again:
	if (t->disk_filename[0])
		unlink(t->disk_filename);
	// recursive call
	task_pop_top_peer(t);
	task_download(t, tracker_task);
}


// task_listen(listen_task)
//	Accepts a connection from some other peer.
//	Returns a TASK_UPLOAD task for the new connection.
static task_t *task_listen(task_t *listen_task)
{
	struct sockaddr_in peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	int fd;
	task_t *t;
	assert(listen_task->type == TASK_PEER_LISTEN);

	fd = accept(listen_task->peer_fd,
		    (struct sockaddr *) &peer_addr, &peer_addrlen);
	if (fd == -1 && (errno == EINTR || errno == EAGAIN
			 || errno == EWOULDBLOCK))
		return NULL;
	else if (fd == -1)
		die("accept");

	message("* Got connection from %s:%d\n",
		inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));

	t = task_new(TASK_UPLOAD);
	t->peer_fd = fd;
	return t;
}


// task_upload(t)
//	Handles an upload request from another peer.
//	First reads the request into the task buffer, then serves the peer
//	the requested file.
static void task_upload(task_t *t)
{
	assert(t->type == TASK_UPLOAD);
	// First, read the request from the peer.
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Cannot read from connection");
			goto exit;
		} else if (ret == TBUF_END
			   || (t->tail && t->buf[t->tail-1] == '\n'))
			break;
	}
	
	

	assert(t->head == 0);
	//protocol for multiple uploads
	//t->flag lets you know that its a multi-upload
	//t->num_blocks lets you know the spacing between blocks to upload (i.e. number of peers)
	//t->block_offset lets you know the offset to start uploading at (i.e. peer number)
	if (osp2p_snscanf(t->buf, t->tail, "GET -%c %i %i %s OSP2P\n", t->flag, t->num_uploading_peers, t->block_offset, t->filename) >= 0) {
		message("Uploading with block offset %d and block spacing of %d", t->block_offset, t->num_uploading_peers);
	}
	else if (osp2p_snscanf(t->buf, t->tail, "GET %s OSP2P\n", t->filename) >= 0) {
		message("Uploading serially");
	} else {
		error("* Odd request %.*s\n", t->tail, t->buf);
		goto exit;
	}
	
	//Exercise 2 code:
	
	int len = t->tail;
	
	//t->filename[len] = '\0'; //Null terminate filename, This might be unecessary
	
	 //Detect too long file name
	if (len > FILENAMESIZ){ //Amount of chars i 'GET %s OSP2P\n'
		error("Filename requested is too long");
		goto exit;
	}
	
	int i;
	for (i = 1; i < len; i++) 
	{
		if (t->filename[i] == '/')
		{
			error("Peer attempting to access outside working directory");
			goto exit;
		}
	}

	//End of exercise 2 code
	
	t->head = t->tail = 0;

	t->disk_fd = open(t->filename, O_RDONLY);
	if (t->disk_fd == -1) {
		error("* Cannot open file %s", t->filename);
		goto exit;
	}
	
	
	//Instead of writing to the tasks peer_fd, we store this on the peers file_fd instead.

	
	if (t->flag) //Run upload for multiple peers
	{
		peer_t *current = t->multi_peer_list;
		while(current != NULL) 
		{
			if (current->block_offset == t->block_offset)
				break;
			current = current->next;
		}
		
		message("* Transferring file %s from peer %d\n", t->filename, t->block_offset);
		
		while (1) 
		{
			int ret = write_from_taskbuf2(current->file_fd, t);
			if (ret == TBUF_ERROR) {
				error("* Peer write error");
				goto exit;
			}

			ret = read_to_taskbuf2(t->disk_fd, t);
			if (ret == TBUF_ERROR) {
				error("* Disk read error");
				goto exit;
			} else if (ret == TBUF_END && t->head == t->tail)
				/* End of file */
				break;
			}
	} 
	else //Do the standard procedure
	{
		message("* Transferring file %s\n", t->filename);
		// Now, read file from disk and write it to the requesting peer.
		while (1) 
		{
			int ret = write_from_taskbuf(t->peer_fd, t);
			if (ret == TBUF_ERROR) {
				error("* Peer write error");
				goto exit;
			}

			ret = read_to_taskbuf(t->disk_fd, t);
			if (ret == TBUF_ERROR) {
				error("* Disk read error");
				goto exit;
			} else if (ret == TBUF_END && t->head == t->tail)
				/* End of file */
				break;
		}
	}
	message("* Upload of %s complete\n", t->filename);

    exit:
	task_free(t);
}


// main(argc, argv)
//	The main loop!
int main(int argc, char *argv[])
{
	task_t *tracker_task, *listen_task, *t;
	struct in_addr tracker_addr;
	int tracker_port;
	char *s;
	const char *myalias;
	struct passwd *pwent;

	osp2p_sscanf("164.67.100.231:12997", "%I:%d",
		     &tracker_addr, &tracker_port);
	if ((pwent = getpwuid(getuid()))) {
		myalias = (const char *) malloc(strlen(pwent->pw_name) + 20);
		sprintf((char *) myalias, "%s%d", pwent->pw_name,
			(int) time(NULL));
	} else {
		myalias = (const char *) malloc(40);
		sprintf((char *) myalias, "osppeer%d", (int) getpid());
	}

	// Ignore broken-pipe signals: if a connection dies, server should not
	signal(SIGPIPE, SIG_IGN);

	// Process arguments
    argprocess:
	if (argc >= 3 && strcmp(argv[1], "-t") == 0
	    && (osp2p_sscanf(argv[2], "%I:%d", &tracker_addr, &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%d", &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%I", &tracker_addr) >= 0)
	    && tracker_port > 0 && tracker_port <= 65535) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't'
		   && (osp2p_sscanf(argv[1], "-t%I:%d", &tracker_addr, &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%d", &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%I", &tracker_addr) >= 0)
		   && tracker_port > 0 && tracker_port <= 65535) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
		if (chdir(argv[2]) == -1)
			die("chdir");
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'd') {
		if (chdir(argv[1]+2) == -1)
			die("chdir");
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && (strcmp(argv[1], "--help") == 0
				 || strcmp(argv[1], "-h") == 0)) {
		printf("Usage: osppeer [-tADDR:PORT | -tPORT] [-dDIR] [-b]\n"
"Options: -tADDR:PORT  Set tracker address and/or port.\n"
"         -dDIR        Upload and download files from directory DIR.\n"
);
		exit(0);
	}

	// Connect to the tracker and register our files.
	tracker_task = start_tracker(tracker_addr, tracker_port);
	listen_task = start_listen();
	register_files(tracker_task, myalias);
	
	pid_t pid;
	int status;
	int num_downloads = 0, num_uploads = 0;
	

	// First, download files named on command line.
	for (; argc > 1; argc--, argv++)
	{
		if ((t = start_download(tracker_task, argv[1]))) 
		{
			if ( (pid = fork()) == 0)		//Child Process
			{			
				task_download(t, tracker_task);
				_exit(0);
			}
			else //Parent keeps track of number of downloads
				num_downloads++;
		}
	}
	
	while (num_downloads > 0) 
	{
		waitpid(-1, &status, 0);
		num_downloads--;
	}


	task_t *prev = NULL;
	// Then accept connections from other peers and upload files to them!
	while ((t = task_listen(listen_task)))
	{
		if (prev != NULL && prev->peer_fd == t->peer_fd) //Make sure task isn't serving the same file over and over.
		{
			error("Peer requested Repeated Task");
			continue;
		}
		
		if ( (pid = fork()) == 0)
		{
			task_upload(t);
			_exit(0);
		}

		
		prev = t;
	}
	

	
	
	return 0;
}
