#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
  #define PORT 50007
#endif
#define MAX_QUEUE 5

/* Reads from a client who has entered their name given that there is something to read.
 * Handles the possibility of disconnecting */
int read_client(struct client *p, struct game_state *game);

/* Attempts to find a new line from the input buffer of a given client and sets
 * in_ptr accordingly. */
int find_newline(struct client *p);

/* Writes a message to a specific client who has already entered their name.
 * Handles the possibility of disconnecting. */
void write_client(struct client *p, char *msg, struct game_state *game);

/* Prints the status message to a given client */
void print_status(struct client *p, struct game_state *game);

/* Deals with the message handling when a player attempts to guess */
void handle_guess(struct client *p, struct game_state *game, char guess, char *dict);

void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

/* Prints a message to all clients */
void broadcast(struct game_state *game, char *outbuf);

/* Broadcasts the turn to all clients and the server */
void announce_turn(struct game_state *game);

/* Announces the winner of the game - with a winner specified */
void announce_winner(struct game_state *game, struct client *winner);

/* Move the has_next_turn pointer to the next active client if the flag is set,
 * and announces turn */
void advance_turn(struct game_state *game, int flag);

/* Checks if the game is over following each turn */
int check_game_over(struct client *p, struct game_state *game, char *dictionary);

/* Moves a player from the new_players list to the game list after an
 * acceptable name is inputted.
 * Requires pointer p to be present in the new_players linked list. */
void move_new_player(struct client **new_players, struct client **game_head,
		     struct client *p);

/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

void announce_winner(struct game_state *game, struct client *winner) {
  char win_msg[MAX_MSG];
  strcpy(win_msg, "Game over. ");
  strcat(win_msg, winner->name);
  strcat(win_msg, " won.\n");
  for (struct client *p = game->head; p != NULL; p = p->next) {
    if (p == winner) {
      write_client(p, "Game over! You win!\n", game);
    } else {
      write_client(p, win_msg, game);
    }
  }
  printf("%s", win_msg);
}

int check_game_over(struct client *p, struct game_state *game, char *dictionary) {
  char word[MAX_MSG];
  strcpy(word, "The word was ");
  strcat(word, game->word);
  strcat(word, ".\n");
  if (strcmp(game->word, game->guess) == 0) {
    broadcast(game, word);
    announce_winner(game, p);
  } else if (game->guesses_left == 0) {
    broadcast(game, word);
    broadcast(game, "No guesses left. Game over.\n");
  } else {
    return 0;
  }
  init_game(game, dictionary);
  printf("New game\n");
  broadcast(game, "\nLet's start a new game\n");
  for (struct client *q = game->head; q != NULL; q = q->next) {
    print_status(q, game);
  }
  return 1;
}

void advance_turn(struct game_state *game, int flag) {
  if (!flag) {
    if (game->has_next_turn->next == NULL) {
      game->has_next_turn = game->head;
    } else {
      game->has_next_turn = game->has_next_turn->next;
    }
  }
  announce_turn(game);
}

void print_status(struct client *p, struct game_state *game) {
  char *status = malloc(MAX_MSG);
  status = status_message(status, game);
  write_client(p, status, game);
  free(status);
}

void close_client(struct client *p, struct game_state *game) {
  char dc[MAX_MSG];
  strcpy(dc, "Goodbye ");
  strcat(dc, p->name);
  strcat(dc, "\n");
  broadcast(game, dc);
  if (game->has_next_turn == p) {
    advance_turn(game, 0);
  } else {
    announce_turn(game);
  }
  printf("Disconnect from %s\n", inet_ntoa(p->ipaddr));
  remove_player(&game->head, p->fd);  
}

void write_client(struct client *p, char *msg, struct game_state *game) {
  int w;
  if ((w = write(p->fd, msg, strlen(msg))) == -1) {
    fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
    close_client(p, game);
  } else if (w == 0) {
    close_client(p, game);
  }
}

int read_client(struct client *p, struct game_state *game) {
  int r;
  char buf[MAX_BUF];
  if ((r = read(p->fd, buf, MAX_BUF)) == -1) {
    fprintf(stderr, "Read from client %s failed\n", inet_ntoa(p->ipaddr));
    remove_player(&game->head, p->fd);
    return -1;
  }
  printf("[%d] Read %d bytes\n", p->fd, r);
  if (r == 0) {
    close_client(p, game);
    return -1;
  } else {
    if (r > MAX_BUF - (p->in_ptr - p->inbuf)) {
      printf("[%d] Exceeded maximum input length\n", p->fd);
      p->in_ptr = p->inbuf;
    }
    for (int i = 0; i < r; i++) {
      p->in_ptr[i] = buf[i];
    }
    p->in_ptr[r] = '\0';
    p->in_ptr = &(p->in_ptr[r]);
  }
  return 0;
}

int find_newline(struct client *p) {
  for (int i = 0; i < MAX_BUF - 1; i++) {
    if (p->inbuf[i] == '\0') {
      p->in_ptr = &(p->inbuf[i]);
      break;
    }
    if (p->inbuf[i] == '\r' && p->inbuf[i+1] == '\n') {
      p->inbuf[i] = '\0';
      p->in_ptr = p->inbuf;
      printf("[%d] Found newline %s\n", p->fd, p->inbuf);
      return 1;
    }
  }
  return 0;
}

void handle_guess(struct client *p, struct game_state *game, char guess, char *dict) {
  int flag = 0;
  if (!(guess >= 'a' && guess <= 'z')) {
    char *out_of_bounds = "Invalid guess, please try again.\n";
    write_client(p, out_of_bounds, game);
  } else if (game->letters_guessed[guess - 'a']) {
    char *already_guessed = "Letter was already guessed, please try again.\n";
    write_client(p, already_guessed, game);
  } else {
    if (process_guess(game, guess)) {
      flag = 1;
    } else {
      char not_in_word[MAX_MSG];
      not_in_word[0] = guess;
      not_in_word[1] = '\0';
      strcat(not_in_word, " is not in the word\n");
      printf("%s", not_in_word);
      if (write(p->fd, not_in_word, strlen(not_in_word)) == -1) {
	fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
      }
    }
    char guess_msg[MAX_MSG];
    strcpy(guess_msg, p->name);
    strcat(guess_msg, " guesses: ");
    guess_msg[strlen(guess_msg) + 2] = '\0';
    guess_msg[strlen(guess_msg) + 1] = '\n';
    guess_msg[strlen(guess_msg)] = guess;
    broadcast(game, guess_msg);
    if (!check_game_over(p, game, dict)) {
      char *status = malloc(MAX_MSG);
      status = status_message(status, game);
      broadcast(game, status);
      free(status);
    }
    advance_turn(game, flag);		      
  }
}

void announce_turn(struct game_state *game) {
  char turn[MAX_MSG];
  strcpy(turn, "It's ");
  strcat(turn, game->has_next_turn->name);
  strcat(turn, "'s turn.\n");
  for (struct client *p = game->head; p != NULL; p = p->next) {
    if (p == game->has_next_turn) {
      char *your_guess = "Your guess?\n";
      if (write(p->fd, your_guess, strlen(your_guess)) == -1) {
	fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
      }
    } else {
      if (write(p->fd, turn, strlen(turn)) == -1) {
	fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
      }
    }
  }
  printf("%s", turn);
}

void broadcast(struct game_state *game, char *outbuf) {
  for (struct client *p = game->head; p != NULL; p = p->next) {
    if (write(p->fd, outbuf, strlen(outbuf)) == -1) {
      fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
    }
  }
}

/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
  struct client *p = malloc(sizeof(struct client));

  if (!p) {
    perror("malloc");
    exit(1);
  }

  printf("Adding client %s\n", inet_ntoa(addr));

  p->fd = fd;
  p->ipaddr = addr;
  p->name[0] = '\0';
  p->in_ptr = p->inbuf;
  p->inbuf[0] = '\0';
  p->next = *top;
  *top = p;
}

void move_new_player(struct client **new_players, struct client **game_head,
		     struct client *p) {
  for (struct client *q = (*new_players); q != NULL; q = q->next) {
    if (q->next == p) {
      q->next = p->next;
    } else if (q == p) {
      *new_players = p->next;
    }
  }
  struct client *temp = *game_head;
  *game_head = p;
  p->next = temp;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset
 */
void remove_player(struct client **top, int fd) {
  struct client **p;

  for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
    ;
  // Now, p points to (1) top, or (2) a pointer to another client
  // This avoids a special case for removing the head of the list
  if (*p) {
    struct client *t = (*p)->next;
    printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
    FD_CLR((*p)->fd, &allset);
    close((*p)->fd);
    free(*p);
    *p = t;
  } else {
    fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
	    fd);
  }
}

int main(int argc, char **argv) {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPIPE, &sa, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }
  
  int clientfd, maxfd, nready;
  struct client *p;
  struct sockaddr_in q;
  fd_set rset;

  if(argc != 2){
    fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
    exit(1);
  }

  // Create and initialize the game state
  struct game_state game;

  srandom((unsigned int)time(NULL));
  // Set up the file pointer outside of init_game because we want to
  // just rewind the file when we need to pick a new word
  game.dict.fp = NULL;
  game.dict.size = get_file_length(argv[1]);

  init_game(&game, argv[1]);

  // head and has_next_turn also don't change when a subsequent game is
  // started so we initialize them here.
  game.head = NULL;
  game.has_next_turn = NULL;

  /* A list of client who have not yet entered their name.  This list is
   * kept separate from the list of active players in the game, because
   * until the new playrs have entered a name, they should not have a turn
   * or receive broadcast messages.  In other words, they can't play until
   * they have a name.
   */
  struct client *new_players = NULL;

  struct sockaddr_in *server = init_server_addr(PORT);
  int listenfd = set_up_server_socket(server, MAX_QUEUE);

  // initialize allset and add listenfd to the
  // set of file descriptors passed into select
  FD_ZERO(&allset);
  FD_SET(listenfd, &allset);
  // maxfd identifies how far into the set to search
  maxfd = listenfd;

  while (1) {
    // make a copy of the set before we pass it into select
    rset = allset;
    nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
    if (nready == -1) {
      perror("select");
      continue;
    }

    if (FD_ISSET(listenfd, &rset)){
      printf("A new client is connecting\n");
      clientfd = accept_connection(listenfd);

      FD_SET(clientfd, &allset);
      if (clientfd > maxfd) {
	maxfd = clientfd;
      }
      printf("Connection from %s\n", inet_ntoa(q.sin_addr));
      add_player(&new_players, clientfd, q.sin_addr);
      char *greeting = WELCOME_MSG;
      if(write(clientfd, greeting, strlen(greeting)) == -1) {
	fprintf(stderr, "Write to client %s failed\n",
		inet_ntoa(q.sin_addr));
	remove_player(&(new_players), p->fd);
      };
    }

    /* Check which other socket descriptors have something ready to read.
     * The reason we iterate over the rset descriptors at the top level and
     * search through the two lists of clients each time is that it is
     * possible that a client will be removed in the middle of one of the
     * operations. This is also why we call break after handling the input.
     * If a client has been removed the loop variables may not longer be
     * valid.
     */
    int cur_fd;
    for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
      if(FD_ISSET(cur_fd, &rset)) {

	//Handling active clients
	for(p = game.head; p != NULL; p = p->next) {
	  if(cur_fd == p->fd) {

	    //Out of turn inputs
	    if (cur_fd != game.has_next_turn->fd) {
	      if (read_client(p, &game) == -1) {
		break;
	      }					      
	      char *out_of_turn = "It is not your turn.\n";
	      write_client(p, out_of_turn, &game);
	      char srv_out_of_turn[MAX_MSG];
	      strcpy(srv_out_of_turn, "Player ");
	      strcat(srv_out_of_turn, p->name);
	      strcat(srv_out_of_turn, " tried to guess out of turn\n");
	      printf("%s", srv_out_of_turn);

	    } else { //Guess handling
	      if (read_client(p, &game) == -1) {
		break;
	      }
	      if (find_newline(p)) {
		if (strlen(p->inbuf) == 1) {
		  char guess = p->inbuf[0];
		  handle_guess(p, &game, guess, argv[1]);
		} else {
		  write_client(p, "Invalid guess, please try again.\n", &game);
		}
	      }
	    }
	    
	    break;
	  }
	}

	// Handling name inputs
	for(p = new_players; p != NULL; p = p->next) {
	  if(cur_fd == p->fd) {
	    int r;
	    char buf[MAX_BUF];
	    if ((r = read(p->fd, buf, MAX_BUF)) == -1) {
	      fprintf(stderr, "Read from client %s failed\n", inet_ntoa(p->ipaddr));
	      remove_player(&new_players, p->fd);
	      break;
	    }
	    printf("[%d] Read %d bytes\n", p->fd, r);
	    if (r == 0) {
	      printf("Disconnect from %s\n", inet_ntoa(p->ipaddr));
	      remove_player(&new_players, p->fd);
	      break;
	    } else {
	      if (r > MAX_BUF - (p->in_ptr - p->inbuf)) {
		printf("[%d] Exceeded maximum input length\n", p->fd);
		p->in_ptr = p->inbuf;
	      }
	      for (int i = 0; i < r; i++) {
		p->in_ptr[i] = buf[i];
	      }
	      p->in_ptr[r] = '\0';
	      p->in_ptr = &(p->in_ptr[r]);
	    }

	    if (find_newline(p)) {
	      int cmp_flag = 0;
	      for (struct client* q = game.head; q != NULL; q = q->next) {
		if (strcmp(q->name, p->inbuf) == 0) {
		  cmp_flag = 1;
		}
	      }
	      if (cmp_flag || strlen(p->inbuf) == 0 || strlen(p->inbuf) >= MAX_NAME) {
		char *bad_name = "Name was not acceptable, please try again.\n";
		int w;
		if ((w = write(p->fd, bad_name, strlen(bad_name))) == -1) {
		  fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
		  remove_player(&new_players, p->fd);
		  break;
		} else if (w == 0) {
		  printf("Disconnect from %s\n", inet_ntoa(p->ipaddr));
		  remove_player(&new_players, p->fd);
		  break;
		}
	      } else {
		strcpy(p->name, p->inbuf);
		move_new_player(&new_players, &game.head, p);
		char join[MAX_MSG];
		strcpy(join, p->name);
		strcat(join, " has just joined.\n");
		printf("%s", join);
		broadcast(&game, join);
		print_status(p, &game);
		if (game.has_next_turn == NULL) {
		  game.has_next_turn = p;
		}
		announce_turn(&game);
	      }
	    }
	    break;
	  }
	}
      }
    }
  }
  return 0;
}
