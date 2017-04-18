#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <time.h>
#include <stdlib.h>

#define PROTOCOL_VERSION 1

#define MESSAGE_JOIN 1
#define MESSAGE_LEAVE 2
#define MESSAGE_REFRESH 3
#define MESSAGE_TEXT 4
#define MESSAGE_ACKNOWLEDGE 5
#define MESSAGE_ERROR 255

#define ERROR_UNKNOWN_ERROR 0
#define ERROR_UNKNOWN_MESSAGE 1
#define ERROR_SEQUENCE 2
#define ERROR_PARAMETER 3

#define POLL_TIMEOUT 5*60*1000
#define EXPIRE_TIMEOUT 5*60

#define MAXUSERS 100

#if defined(DEBUG)
#define TRACE0(msg) do { fprintf(stderr, msg); fprintf(stderr, "\n"); } while(0)
#define TRACE(msg, param) do { fprintf(stderr, msg, param); fprintf(stderr, "\n"); } while(0)
#else
#define TRACE0(msg)
#define TRACE(msg, param)
#endif

struct user {
    int active;
    char name[40];
    struct sockaddr_storage addr;
    time_t last_seen;
    unsigned char seqno;
};

struct user users[MAXUSERS];
int sockfd;

int allocate()
{
    int i;
    for (i = 0; i < MAXUSERS; i++) {
        if (!users[i].active) {
            users[i].active = 1;
            return i;
        }
    }
    return -1;
}

int find_user(char *name)
{
    int i;
    for(i = 0; i < MAXUSERS; i++) {
        if (users[i].active && strcmp(users[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void expire()
{
    time_t now = time(NULL);
    int i, k;
    char msg[50] = {PROTOCOL_VERSION, MESSAGE_LEAVE};
    size_t len;
    for (i = 0; i < MAXUSERS; i++) {
        if (users[i].active && users[i].last_seen < now-EXPIRE_TIMEOUT) {
            TRACE("expiring %s", users[i].name);
            users[i].active = 0;
            strcpy(msg+2, users[i].name);
            len = 2 + strlen(users[i].name) + 1;
            // notify remaining users
            for (k = 0; k < MAXUSERS; k++) {
                if (!users[k].active) continue;
                sendto(sockfd, msg, len, 0, (struct sockaddr*)&users[k].addr, sizeof(struct sockaddr_in));
            }
        }
    }
}

void send_error(struct sockaddr_storage *from, char code)
{
    char msg[] = {PROTOCOL_VERSION, MESSAGE_ERROR, code};
    TRACE0("Sending error");
    sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr*)from, sizeof(struct sockaddr_in));
}


void create_checkpoint()
{
	FILE *f;
	f = fopen ("userbackup", "wb");
	if (f == NULL)
		{
		  TRACE0("Fehler beim Erstellen des Backups.");
		}
	fwrite(&users, sizeof(users),1,f);
	fclose (f);
	TRACE0("Checkpoint erstellt/aktualisiert!");
}

void restore_checkpoint()
{
  FILE *f;
  f = fopen ("userbackup", "rb");
  if (f != NULL)
    {
      fread(&users, sizeof(users), 1,f);
      TRACE0("Checkpoint wiederhergestellt!");
    }
  else 
    {
      TRACE0("Fehler beim Wiederherstellen des Backups.");
    }
    fclose(f);
}	

void do_join(struct sockaddr_storage *from, char* message, ssize_t len)
{
    int slot;
    int i;
    ssize_t res;
    char msg2[50] = {PROTOCOL_VERSION, MESSAGE_JOIN};
    size_t msg2len;
    slot = allocate();
    users[slot].addr = *from;
    strcpy(users[slot].name, message+2);
    users[slot].last_seen = time(NULL);
    users[slot].seqno = 0;
    TRACE("JOIN of %s", users[slot].name);
    // send new user to all others
    for (i = 0; i < MAXUSERS; i++) {
        if (!users[i].active)
            continue;
        res = sendto(sockfd, message, len, 0, (struct sockaddr*)&users[i].addr, sizeof(struct sockaddr_in));
        if (res < 0) {
            perror("sendto");
            exit(1);
        }
    }
    // send all others to new user
    for (i = 0; i < MAXUSERS; i++) {
        if (i == slot || !users[i].active)
            continue;
        msg2len = 2 + strlen(users[i].name) + 1;
        strcpy(msg2+2, users[i].name);
        res = sendto(sockfd, msg2, msg2len, 0, (struct sockaddr*)&users[slot].addr, sizeof(struct sockaddr_in));
        if (res < 0) {
            perror("sendto");
            exit(1);
        }        
    }
}

void do_leave(struct sockaddr_storage *from, char* message, ssize_t len)
{
    int slot;
    int i;
    ssize_t res;
    slot = find_user(message+2);
    if (slot == -1) {
        // not currently a user
        return;
    }
    TRACE("LEAVE of %s", users[slot].name);
    // send new user to all others
    for (i = 0; i < MAXUSERS; i++) {
        if (!users[i].active)
            continue;
        res = sendto(sockfd, message, len, 0, (struct sockaddr*)&users[i].addr, sizeof(struct sockaddr_in));
        if (res < 0) {
            perror("sendto");
            exit(1);
        }
    }
    users[slot].active = 0;
}

void do_refresh(struct sockaddr_storage *from, char* message, ssize_t len)
{
    int i;
    for (i = 0; i < MAXUSERS; i++) {
        if (memcmp(from, &users[i].addr, sizeof(struct sockaddr_in)) == 0) {
            break;
        }
    }
    if (i == MAXUSERS) {
        // not found
        send_error(from, ERROR_SEQUENCE);
        return;
    }
    users[i].last_seen = time(NULL);
}
void do_text(struct sockaddr_storage *from, char *message, ssize_t len)
{
    unsigned char messageid = message[2];
    char *name = message+3;
    int namelen = strlen(name);
    char *data = message+3+namelen+1; 
    int sender = find_user(name);
    int i;
    ssize_t res;
    if (sender == -1) {
        // have not seen prior join
        send_error(from, ERROR_SEQUENCE);
    }
    if (users[sender].seqno != messageid) {
        TRACE("Unexpected message from %s discarded", name);
        return;
    }
    TRACE("TEXT from %s", name);
    for (i = 0; i < MAXUSERS; i++) {
        if (i == sender)
            continue;
        if (!users[i].active)
            continue;
        res = sendto(sockfd, message, len, 0, (struct sockaddr*)&users[i].addr, sizeof(struct sockaddr_in));
        if (res < 0) {
            perror("sendto");
            exit(1);
        }
    }
    // send acknowledge to sender
    char ack[] = {PROTOCOL_VERSION, MESSAGE_ACKNOWLEDGE, messageid};
    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)from, sizeof(struct sockaddr_in));
    users[sender].seqno++;
}


int main()
{
    struct sockaddr_in addr, addr2;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(13353);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    sockfd = socket(PF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    socklen_t len = sizeof(addr2);
    getsockname(sockfd, (struct sockaddr*)&addr2, &len);
    restore_checkpoint();
    while (1) {
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);
        char msg[400];
        ssize_t datalen;
        struct pollfd fds[] = {sockfd, POLLIN, 0};

        switch (poll(fds, 1,  POLL_TIMEOUT)) {
        case -1:
            perror("poll");
            return 1;
        case 0:
            // timeout
            expire();
            continue;
        }
        datalen = recvfrom(sockfd, msg, sizeof(msg), 0, (struct sockaddr*)&from, &fromlen);
        expire();
        if (datalen < 0) {
            perror("recvfrom");
            return 1;
        }
        TRACE("command %d received", msg[1]);
        if (msg[0] != PROTOCOL_VERSION || datalen < 2) {
            send_error(&from, ERROR_UNKNOWN_ERROR);
            continue;
        }
        switch (msg[1]) {
        case MESSAGE_JOIN:
            do_join(&from, msg, datalen);
	    create_checkpoint();
            break;
        case MESSAGE_LEAVE:
            do_leave(&from, msg, datalen);
	    create_checkpoint();
            break;
        case MESSAGE_REFRESH:
            do_refresh(&from, msg, datalen);
            break;
        case MESSAGE_TEXT:
            do_text(&from, msg, datalen);
            break;
            //XXX more messages
        default:
            send_error(&from, ERROR_UNKNOWN_MESSAGE);
            break;
        }
    }
}
