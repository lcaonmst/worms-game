/* Screen worms client.
 * Author: Kamil Zwierzchowski.
 * Client is fully operational.
 */

#include <string>
#include <getopt.h>
#include <cstring>
#include <netdb.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unordered_set>
#include "err.h"
#include "common.h"

#define MAX_KEY_LEN 15
#define GUI_BUFFER 1024
#define TIMEOUT 5000
#define SEND_INTERVAL_MS 30LL
#define NS_IN_MS 1000000LL
#define SEND_INTERVAL (SEND_INTERVAL_MS * NS_IN_MS)
#define US_IN_S 1000000LL
#define DEFAULT_GUI_PORT 20210

#define LEFT_KEY_DOWN "LEFT_KEY_DOWN\n"
#define LEFT_KEY_UP "LEFT_KEY_UP\n"
#define RIGHT_KEY_DOWN "RIGHT_KEY_DOWN\n"
#define RIGHT_KEY_UP "RIGHT_KEY_UP\n"
#define NEW_GAME "NEW_GAME "
#define PIXEL "PIXEL "
#define PLAYER_ELIMINATED "PLAYER_ELIMINATED "
#define PROGRAM_FLAGS "n:p:i:r:"
#define USAGE "./screen-worms-client game_server [-n player_name] [-p n] [-i gui_server] [-r n]"
#define INVALID_PLAYER_NAME "invalid player name"
#define INVALID_GUI_PORT "invalid gui port"

/* Data type which represents all previous games numbers.
 */
using old_games_t = std::unordered_set<int32>;

/* Data type which represents all user options.
 */
struct client_flags_t {
    std::string game_server;
    player_name_t player_name;
    int16 port_number = DEFAULT_PORT_NUMBER;
    std::string gui_server = "localhost";
    int16 gui_port_number = DEFAULT_GUI_PORT;
};

/* Data type which represents current state of game.
 */
struct game_data_t {
    int32 game_id;
    int64 session_id;
    int32 next_expected_event = 0;
    int8 last_direction = 0;
    int8 player_num;
    int32 max_x;
    int32 max_y;
    player_name_t player_name[MAX_PLAYER];
    old_games_t old_games;
};

/* Data type which represents message to server.
 */
struct message_to_server_t {
    client_message_t client_message;
    int8 len;
};


/* Function which gets and sets all user options.
 */
static void get_flags(int argc, char **argv, client_flags_t &client_flags) {
    if (argc < 2) {
        fatal(USAGE);
    }
    client_flags.game_server = std::string(argv[1]);
    memset(&client_flags.player_name, 0, MAX_PLAYER_NAME);

    while (true) {
        int8 c = getopt(argc, argv, PROGRAM_FLAGS);
        bool done = false;
        switch (c) {
            case 'n': {
                if (MAX_PLAYER_NAME <= strlen(optarg) || !check_player_name(optarg)) {
                    fatal(INVALID_PLAYER_NAME);
                }
                strcpy(client_flags.player_name, optarg);
                break;
            }
            case 'p': {
                long int port = std::strtol(optarg, nullptr, 10);
                if (errno != 0 || port < 1 || MAX_PORT_NUMBER < port) {
                    fatal(INVALID_SERVER_PORT);
                }
                client_flags.port_number = port;
                break;
            }
            case 'i': {
                client_flags.gui_server = std::string(optarg);
                break;
            }
            case 'r': {
                long int port = std::strtol(optarg, nullptr, 10);
                if (errno != 0 || port < 1 || MAX_PORT_NUMBER < port) {
                    fatal(INVALID_GUI_PORT);
                }
                client_flags.gui_port_number = port;
                break;
            }
            case 255: {
                done = true;
                break;
            }
            default: {
                fatal(USAGE);
            }
        }
        if (done) {
            break;
        }
    }
}

/* Function which sets session.
 */
static void set_session(game_data_t &game_data) {
    timeval curr_time;
    memset(&curr_time, 0, sizeof(curr_time));
    if (gettimeofday(&curr_time, nullptr) != 0) {
        syserr("cannot get current time");
    }
    game_data.session_id = curr_time.tv_sec * US_IN_S + curr_time.tv_usec;
}

/* Function which sets ind to event which has descriptor equal to desc.
 */
static void set_pointer(epoll_event *events, int8 &ind, int16 desc) {
    for (int8 i = 0; i < 3; i++) {
        if (events[i].data.fd == desc) {
            ind = i;
            return;
        }
    }
}

/* Function which create message to server.
 */
static void create_message_to_server(message_to_server_t &message_to_server,
                              client_flags_t &client_flags, game_data_t &game_data) {

    client_message_t *client_message = &message_to_server.client_message;
    client_message->session_id = htobe64(game_data.session_id);
    client_message->turn_direction = 0;
    strcpy(client_message->player_name, client_flags.player_name);
    client_message->next_expected_event_no = 0;
    message_to_server.len = sizeof(client_message_t) - MAX_PLAYER_NAME
                            + strlen(client_message->player_name);
}

/* Function which sends message to server.
 */
static bool send_to_server(int16 sock, message_to_server_t &message_to_server,
                    game_data_t &game_data) {

    message_to_server.client_message.turn_direction = game_data.last_direction;
    message_to_server.client_message.next_expected_event_no = htonl(game_data.next_expected_event);
    return send(sock, (void *) &message_to_server.client_message,
                message_to_server.len, 0) == message_to_server.len;
}

/* Function that checks if control sum of server event is correct.
 */
static bool check_control_sum(char const *data, int32 len, int32 crc32) {
    return crc32 == crc32_compute(data, len);
}

/* Function that handles message from server.
 */
static void handle_server_message(char const *data, int16 len, game_data_t &game_data,
                           int16 gui_socket, char *sending_data) {

    if (len < 4 || MAX_DATAGRAM < len) {
        return;
    }
    int32 game_id = ntohl(*((int32 *) data));
    if (game_data.old_games.count(game_id) > 0) {
        return;
    }
    if (game_id != game_data.game_id) {
        game_data.game_id = game_id;
        game_data.next_expected_event = 0;
    }
    const char *ptr = data + 4;
    while (ptr - data <= len - 13) {
        const auto *event = (event_t const *)ptr;
        int32 event_no = ntohl(event->event_no);
        int8 event_type = event->event_type;
        int32 curr_len = ntohl(event->len);
        int32 curr_crc32 = ntohl(*((int32 *) (ptr + 4 + curr_len)));
        if (!check_control_sum(ptr, curr_len + 4, curr_crc32)) {
            ptr += curr_len + 8;
            continue;
        }

        ptr += curr_len + 8;
        if (event_no > game_data.next_expected_event) {
            return;
        }
        if (event_no < game_data.next_expected_event) {
            continue;
        }

        char *ptr_to_message = sending_data;
        switch (event_type) {
            case NEW_GAME_EVENT_TYPE: {
                if (curr_len < 13) {
                    syserr("invalid event length");
                }
                if (event_no > 0) {
                    syserr("invalid new_game event");
                }
                const new_game_event_data_t *event_data = &event->event_data.new_game_data;
                strcpy(ptr_to_message, NEW_GAME);
                ptr_to_message += strlen(NEW_GAME);

                game_data.max_x = ntohl(event_data->max_x);
                std::string s = std::to_string(ntohl(event_data->max_x));
                strcpy(ptr_to_message, s.c_str());
                ptr_to_message[s.length()] = ' ';
                ptr_to_message += s.length() + 1;

                game_data.max_y = ntohl(event_data->max_y);
                s = std::to_string(ntohl(event_data->max_y));
                strcpy(ptr_to_message, s.c_str());
                ptr_to_message[s.length()] = ' ';
                ptr_to_message += s.length() + 1;

                int8 all_name_len = curr_len - 13;
                memcpy(ptr_to_message, event_data->all_players, all_name_len);
                int8 player_len = 0;
                int8 player_num = 0;
                for (int8 i = 0; i < all_name_len; i++) {
                    game_data.player_name[player_num][player_len] = ptr_to_message[i];
                    if (ptr_to_message[i] == '\0') {
                        player_len = 0;
                        if (i + 1 < all_name_len) {
                            ptr_to_message[i] = ' ';
                        }
                        else {
                            ptr_to_message[i] = '\n';
                        }
                        player_num++;
                    }
                    else {
                        player_len++;
                    }
                    if (player_len > 20 || player_num > 25) {
                        syserr("invalid players names");
                    }
                }
                game_data.player_num = player_num;
                if (ptr_to_message[all_name_len - 1] != '\n') {
                    syserr("invalid players names");
                }
                if (player_num < 2) {
                    syserr("invalid players names");
                }
                for (int8 i = 0; i + 1 < player_num; i++) {
                    if (strcmp(game_data.player_name[i], game_data.player_name[i + 1]) >= 0) {
                        syserr("invalid players names");
                    }
                }
                ptr_to_message += all_name_len;

                if (write(gui_socket, sending_data, ptr_to_message - sending_data) !=
                    ptr_to_message - sending_data) {

                    syserr("cannot write at gui socket");
                }
                break;
            }
            case PIXEL_EVENT_TYPE: {
                if (curr_len != 14) {
                    syserr("invalid event length");
                }
                const pixel_event_data_t *event_data = &event->event_data.pixel_data;
                if (ntohl(event_data->x) >= game_data.max_x ||
                    ntohl(event_data->y) >= game_data.max_y) {

                    syserr("invalid pixel event");
                }
                strcpy(ptr_to_message, PIXEL);
                ptr_to_message += strlen(PIXEL);

                std::string s = std::to_string(ntohl(event_data->x));
                strcpy(ptr_to_message, s.c_str());
                ptr_to_message[s.length()] = ' ';
                ptr_to_message += s.length() + 1;

                s = std::to_string(ntohl(event_data->y));
                strcpy(ptr_to_message, s.c_str());
                ptr_to_message[s.length()] = ' ';
                ptr_to_message += s.length() + 1;

                if (event_data->player_number >= game_data.player_num) {
                    syserr("invalid player number");
                }
                strcpy(ptr_to_message, game_data.player_name[event_data->player_number]);
                int8 player_len = strlen(game_data.player_name[event_data->player_number]);
                ptr_to_message += player_len;
                ptr_to_message[0] = '\n';
                ptr_to_message++;

                if (write(gui_socket, sending_data, ptr_to_message - sending_data) !=
                    ptr_to_message - sending_data) {

                    syserr("cannot write at gui socket");
                }
                break;
            }
            case PLAYER_ELIMINATED_EVENT_TYPE: {
                if (curr_len != 6) {
                    syserr("invalid event length");
                }
                const player_eliminated_event_data_t *event_data =
                        &event->event_data.player_eliminated_data;
                strcpy(ptr_to_message, PLAYER_ELIMINATED);
                ptr_to_message += strlen(PLAYER_ELIMINATED);

                if (event_data->player_number >= game_data.player_num) {
                    syserr("invalid player number");
                }
                strcpy(ptr_to_message, game_data.player_name[event_data->player_number]);
                int8 player_len = strlen(game_data.player_name[event_data->player_number]);
                ptr_to_message += player_len;
                ptr_to_message[0] = '\n';
                ptr_to_message++;

                if (write(gui_socket, sending_data, ptr_to_message - sending_data) !=
                    ptr_to_message - sending_data) {

                    syserr("cannot write at gui socket");
                }
                break;
            }
            case GAME_OVER_EVENT_TYPE: {
                game_data.old_games.insert(game_data.game_id);
                game_data.next_expected_event = -1;
                break;
            }
            default: {
                break;
            }
        }
        game_data.next_expected_event++;
    }
}

/* Function that handles message from gui.
 */
static void handle_gui_message(int16 gui_sock, game_data_t &game_data, char *data) {
    ssize_t len = read(gui_sock, data, GUI_BUFFER - 24);
    if (len < 0) {
        syserr("cannot read from gui socket");
    }
    data[len] = '\0';
    char *ptr = data;
    int8 curr_len = 0;
    if (len > MAX_KEY_LEN) {
        for (ptr = data + len - 2; *ptr != '\n'; ptr--) {
            curr_len++;
            if (curr_len > MAX_KEY_LEN) {
                syserr("cannot parse gui message");
            }
        }

        ptr++;
        len -= ptr - data;
    }

    if (len < 12) {
        ssize_t new_len = read(gui_sock, ptr + len, 12 - len);
        if (new_len < 0) {
            syserr("cannot read from gui socket");
        }
        len += new_len;
    }
    if (memcmp(ptr, LEFT_KEY_DOWN, 12) == 0) {
        if ((size_t)len < strlen(LEFT_KEY_DOWN) &&
            read(gui_sock, ptr + len, strlen(LEFT_KEY_DOWN) - 12) !=
            strlen(LEFT_KEY_DOWN) - 12) {

            syserr("cannot read from gui socket");
        }
        if (strcmp(ptr, LEFT_KEY_DOWN) != 0) {
            syserr("cannot parse gui message");
        }
        game_data.last_direction = 2;
    }
    else if (memcmp(ptr, LEFT_KEY_UP, 12) == 0) {
        if ((size_t)len < strlen(LEFT_KEY_UP) &&
            read(gui_sock, ptr + len, strlen(LEFT_KEY_UP) - 12) !=
            strlen(LEFT_KEY_UP) - 12) {

            syserr("cannot read from gui socket");
        }
        if (strcmp(ptr, LEFT_KEY_UP) != 0) {
            syserr("cannot parse gui message");
        }
        if (game_data.last_direction == 2) {
            game_data.last_direction = 0;
        }
    }
    else if (memcmp(ptr, RIGHT_KEY_DOWN, 12) == 0) {
        if ((size_t)len < strlen(RIGHT_KEY_DOWN) &&
            read(gui_sock, ptr + len, strlen(RIGHT_KEY_DOWN) - 12) !=
            strlen(RIGHT_KEY_DOWN) - 12) {

            syserr("cannot read from gui socket");
        }
        if (strcmp(ptr, RIGHT_KEY_DOWN) != 0) {
            syserr("cannot parse gui message");
        }
        game_data.last_direction = 1;
    }
    else if (memcmp(ptr, RIGHT_KEY_UP, 12) == 0) {
        if ((size_t)len < strlen(RIGHT_KEY_UP) &&
            read(gui_sock, ptr + len, strlen(RIGHT_KEY_UP) - 12) !=
            strlen(RIGHT_KEY_UP) - 12) {

            syserr("cannot read from gui socket");
        }
        if (strcmp(ptr, RIGHT_KEY_UP) != 0) {
            syserr("cannot parse gui message");
        }
        if (game_data.last_direction == 1) {
            game_data.last_direction = 0;
        }
    }
}

int main(int argc, char **argv) {
    client_flags_t client_flags;
    get_flags(argc, argv, client_flags);

    addrinfo hints_gui;
    addrinfo *result, *rp;
    int sfd = 0, s;
    memset(&hints_gui, 0, sizeof(hints_gui));
    hints_gui.ai_family = AF_UNSPEC;
    hints_gui.ai_socktype = SOCK_STREAM;
    hints_gui.ai_protocol = IPPROTO_TCP;

    s = getaddrinfo(client_flags.gui_server.c_str(),
                    std::to_string(client_flags.gui_port_number).c_str(),
                    &hints_gui, &result);

    if (s != 0) {
        syserr("cannot receive ip address");
    }
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            continue;
        }

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sfd);
    }
    freeaddrinfo(result);
    if (rp == nullptr) {
        syserr("cannot find fitting ip address for gui");
    }
    RaiiDescriptor sock_gui{sfd};
    int32 flag = 1;
    s = setsockopt(sfd,IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int32));
    if (s < 0) {
        syserr("cannot disable Nagle algorithm");
    }

    addrinfo hints_server;
    memset(&hints_server, 0, sizeof(hints_server));
    hints_server.ai_family = AF_UNSPEC;
    hints_server.ai_socktype = SOCK_DGRAM;

    s = getaddrinfo(client_flags.game_server.c_str(),
                    std::to_string(client_flags.port_number).c_str(),
                    &hints_server, &result);
    errno = 0;
    if (s != 0) {
        syserr("cannot receive ip address");
    }
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype,
                     rp->ai_protocol);
        if (sfd == -1) {
            continue;
        }

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sfd);
    }
    freeaddrinfo(result);
    if (rp == nullptr) {
        syserr("cannot find fitting ip address for server");
    }
    fcntl(sfd, F_SETFL, O_NONBLOCK);
    RaiiDescriptor sock_server{sfd};

    sfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (sfd < 0) {
        syserr("cannot create timer");
    }
    RaiiDescriptor timer{sfd};
    itimerspec expiration;
    expiration.it_value.tv_sec = 1;
    expiration.it_value.tv_nsec = 1;
    expiration.it_interval.tv_sec = SEND_INTERVAL / NS_IN_S;
    expiration.it_interval.tv_nsec = SEND_INTERVAL % NS_IN_S;

    if (timerfd_settime(sfd, 0, &expiration, nullptr) < 0) {
        syserr("cannot arm the timer");
    }

    sfd = epoll_create(1);
    if (sfd < 0) {
        syserr("cannot create epoll");
    }
    RaiiDescriptor epoll{sfd};

    epoll_event timer_event;
    timer_event.data.fd = timer.descriptor;
    timer_event.events = EPOLLIN | EPOLLERR;
    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_ADD, timer.descriptor, &timer_event) < 0) {
        syserr("cannot add to epoll");
    }

    epoll_event server_event;
    server_event.data.fd = sock_server.descriptor;
    server_event.events = EPOLLIN | EPOLLERR;
    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_ADD, sock_server.descriptor, &server_event) < 0) {
        syserr("cannot add to epoll");
    }

    epoll_event gui_event;
    gui_event.data.fd = sock_gui.descriptor;
    gui_event.events = EPOLLIN | EPOLLERR;
    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_ADD, sock_gui.descriptor, &gui_event) < 0) {
        syserr("cannot add to epoll");
    }

    game_data_t game_data;
    set_session(game_data);
    message_to_server_t message_to_server;
    create_message_to_server(message_to_server, client_flags, game_data);


    epoll_event catch_event[3];
    char from_server_data[MAX_DATAGRAM + 1];
    char to_gui_data[2 * MAX_DATAGRAM];
    char from_gui_data[GUI_BUFFER];
    while (true) {
        memset(catch_event, 0, 3 * sizeof(epoll_event));
        ssize_t events = epoll_wait(epoll.descriptor, catch_event, 3, TIMEOUT);
        if (events == 0) {
            syserr("timer not responds");
        }
        if (events < 0) {
            syserr("epoll not responds");
        }

        int8 ind = 3;
        set_pointer(catch_event, ind, sock_gui.descriptor);
        set_pointer(catch_event, ind, timer.descriptor);
        set_pointer(catch_event, ind, sock_server.descriptor);
        if (ind > 2) {
            syserr("epoll error");
        }

        if (catch_event[ind].data.fd == timer.descriptor) {
            if (catch_event[ind].events & EPOLLERR) {
                syserr("timer error");
            }
            int64 tmp;
            if (read(timer.descriptor, &tmp, 8) < 0) {
                syserr("timer error");
            }
            send_to_server(sock_server.descriptor, message_to_server, game_data);
        }
        else if (catch_event[ind].data.fd == sock_server.descriptor) {
            if (catch_event[ind].events & EPOLLERR) {
                syserr("server socket error");
            }
            ssize_t len = recv(sock_server.descriptor, from_server_data,
                             MAX_DATAGRAM + 1, 0);
            if (len < 0) {
                syserr("cannot read from server socket");
            }
            handle_server_message(from_server_data, len, game_data,
                                  sock_gui.descriptor, to_gui_data);
        }
        else if (catch_event[ind].data.fd == sock_gui.descriptor) {
            if (catch_event[ind].events & EPOLLERR) {
                syserr("gui socket error");
            }
            handle_gui_message(sock_gui.descriptor, game_data, from_gui_data);
        }
    }
}