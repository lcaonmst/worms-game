/* Screen-worms-server
 * Author: Kamil Zwierzchowski
 * Server does not support on IPv6
 */

#include <vector>
#include <map>
#include <chrono>
#include <cmath>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <getopt.h>
#include <cstdint>
#include <unistd.h>
#include <queue>
#include <cstring>
#include <endian.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <algorithm>
#include "common.h"
#include "err.h"

#define USAGE "./screen-worms-server [-p port] [-s seed] [-t turning] [-v rounds per s.] [-w width] [-h height]"
#define PROGRAM_FLAGS "p:s:t:v:w:h:"
#define DEFAULT_TURNING_SPEED 6
#define DEFAULT_ROUNDS_PER_SEC 50
#define DEFAULT_HEIGHT 640
#define DEFAULT_WIDTH 480
#define RAND_MULTIPLY 279410273
#define RAND_MODULO 4294967291
#define TIMEOUT 2000
#define MIN_CLIENT_MESSAGE_LEN 13
#define MAX_CLIENT_MESSAGE_LEN (MIN_CLIENT_MESSAGE_LEN + MAX_PLAYER)


/* Data type which represents stream of events, ready to send.
 * It makes it easy to send data to client, it is always consistent interval.
 */
struct raw_events_t {
    int32 events_no = 0;
    int32 free_byte = 4;
    raw_events_data_t raw_events_data;
    raw_events_start_t raw_events_start;
};

/* Data type which represents player.
 * Observer is not considered as player.
 * It is consisted of all necessary information.
 */
struct player_data_t {
    player_name_t player_name;
    int8 player_no;
    point_t cord;
    int32 direction;
    int8 last_direction;
    bool eliminated = true;
    bool connected = false;
};

/* Data type which identifies client.
 */
struct client_id_t {
    int64 session_id;
    int16 port_number;
    char ip_address[INET6_ADDRSTRLEN];
};

/* Data type which represents client.
 * If client is observer, then player points to NULL
 */
struct client_data_t {
    client_id_t client_id;
    player_data_t *player;
    time_point last_activity;
};

/* Data type which represents all active clients.
 */
using clients_t = std::vector<client_data_t>;

/* Data type which represents current state on board.
 * If pixel is included in map, then it is already eaten.
 * Otherwise, it is free.
 */
using board_data_t = std::map<pixel_t, player_data_t*>;

/* Data type which represents all active player.
 */
struct players_t {
    int8 alive_players = 0;
    std::vector<player_data_t*> player_data;
};

/* Data type which includes all program options
 */
struct server_flags_t {
    int16 port_number = DEFAULT_PORT_NUMBER;
    int32 turning_speed = DEFAULT_TURNING_SPEED;
    int32 rounds_per_sec = DEFAULT_ROUNDS_PER_SEC;
    int32 width = DEFAULT_WIDTH;
    int32 height = DEFAULT_HEIGHT;
};

/* Data type which represents current playing game.
 */
struct game_data_t {
    int32 game_id;
    board_data_t board_data;
    server_flags_t *server_flags;
    players_t *players;
    bool is_started = false;
    bool is_ready = false;
};

/* Data type which represents single writing job.
 * It consists of client address and his expected event.
 */
struct write_data_t {
    int32 begin_event;
    sockaddr client_addr;
    socklen_t client_len;
};

/* Data type which represents all writing jobs.
 */
using write_work_t = std::deque<write_data_t>;



/* Function that converts point to pixel.
 */
static pixel_t get_pixel(point_t point) {
    return std::make_pair((int32)point.x, (int32)point.y);
}

/* Function that check current state of pixel.
 * If it is out of border, returns 1.
 * If it is busy by some player, return 2.
 * Otherwise, returns 0.
 */
static int8 get_owner(pixel_t pixel, const game_data_t &game_data) {
    if (pixel.x >= game_data.server_flags->width ||
        pixel.y >= game_data.server_flags->height) {

        return 1;
    }
    if (game_data.board_data.count(pixel) > 0) {
        return 2;
    }
    return 0;
}

/* Function that returns random number, according to its definition.
 */
static int32 custom_rand(int32 seed=time(nullptr)) {
    static int32 current_rand = seed;
    static int32 counter = -1;

    counter++;
    int32 res = current_rand;
    if (counter) {
        current_rand = (int64)current_rand * RAND_MULTIPLY % RAND_MODULO;
    }
    return res;
}

/* Function that initializes random number generator.
 */
static void custom_rand_init(int32 seed=time(nullptr)) {
    static bool inited = false;
    if (!inited) {
        custom_rand(seed);
        inited = true;
    }
}

/* Function that compute control sum for last event in raw_events.
 * The control sum is put at the end of raw_events.
 */
static void compute_control_sum(raw_events_t &raw_events) {
    int8 *begin_addr = &raw_events.raw_events_data[raw_events.raw_events_start.back()];
    int32 len = raw_events.free_byte - raw_events.raw_events_start.back();
    int32 crc32 = crc32_compute(begin_addr, len);
    auto ptr = (int32*)&raw_events.raw_events_data[raw_events.free_byte];
    *ptr = htonl(crc32);
    raw_events.free_byte += 4;
}

/* Function that resizes raw_events in case of not enough space.
 */
static void raw_data_resize(raw_events_t &raw_events) {
    if (raw_events.raw_events_data.empty()) {
        raw_events.raw_events_data.resize(4);
    }
    size_t ss = raw_events.raw_events_data.size();
    if (ss - raw_events.free_byte < MAX_DATAGRAM) {
        raw_events.raw_events_data.resize(ss + MAX_DATAGRAM);
    }
}

/* Function that puts all players names into NEW_GAME_EVENT.
 */
static int16 save_players_names(char *ptr, const game_data_t &game_data) {
    int16 len = 0;
    for (auto player : game_data.players->player_data) {
        for (int8 i = 0; i < MAX_PLAYER_NAME; i++) {
            char c = player->player_name[i];
            *ptr = c;
            ptr++;
            len++;
            if (c == '\0') {
                break;
            }
        }
    }
    return len;
}

/* Function that creates NEW_GAME_EVENT and puts it into raw_events.
 */
static void create_new_game_event(const game_data_t &game_data, raw_events_t &raw_events) {
    raw_data_resize(raw_events);
    event_t *event = (event_t*)&raw_events.raw_events_data[raw_events.free_byte];
    event->event_type = NEW_GAME_EVENT_TYPE;
    event->event_no = htonl(raw_events.events_no);
    event->event_data.new_game_data.max_x = htonl(game_data.server_flags->width);
    event->event_data.new_game_data.max_y = htonl(game_data.server_flags->height);
    int32 len = save_players_names(event->event_data.new_game_data.all_players, game_data);
    event->len = htonl(13 + len);
    raw_events.events_no++;
    raw_events.raw_events_start.push_back(raw_events.free_byte);
    raw_events.free_byte += ntohl(event->len) + 4;
    compute_control_sum(raw_events);
}

/* Function that creates PIXEL_EVENT and puts it into raw_events.
 */
static void create_pixel_event(int8 player_no, pixel_t pixel, raw_events_t &raw_events) {
    raw_data_resize(raw_events);
    auto event = (event_t*)&raw_events.raw_events_data[raw_events.free_byte];
    event->event_type = PIXEL_EVENT_TYPE;
    event->event_no = htonl(raw_events.events_no);
    event->event_data.pixel_data.player_number = player_no;
    event->event_data.pixel_data.x = htonl(pixel.x);
    event->event_data.pixel_data.y = htonl(pixel.y);
    event->len = htonl(14);
    raw_events.events_no++;
    raw_events.raw_events_start.push_back(raw_events.free_byte);
    raw_events.free_byte += ntohl(event->len) + 4;
    compute_control_sum(raw_events);
}

/* Function that creates PLAYER_ELIMINATED_EVENT and puts it into raw_events.
 */
static void create_player_eliminated_event(int8 player_no, raw_events_t &raw_events) {
    raw_data_resize(raw_events);
    auto event = (event_t*)&raw_events.raw_events_data[raw_events.free_byte];
    event->event_type = PLAYER_ELIMINATED_EVENT_TYPE;
    event->event_no = htonl(raw_events.events_no);
    event->event_data.player_eliminated_data.player_number = player_no;
    event->len = htonl(6);
    raw_events.events_no++;
    raw_events.raw_events_start.push_back(raw_events.free_byte);
    raw_events.free_byte += ntohl(event->len) + 4;
    compute_control_sum(raw_events);
}

/* Function that creates GAME_OVER_EVENT and puts it into raw_events.
 */
static void create_game_over_event(raw_events_t &raw_events) {
    raw_data_resize(raw_events);
    auto event = (event_t*)&raw_events.raw_events_data[raw_events.free_byte];
    event->event_type = GAME_OVER_EVENT_TYPE;
    event->event_no = htonl(raw_events.events_no);
    event->len = htonl(5);
    raw_events.events_no++;
    raw_events.raw_events_start.push_back(raw_events.free_byte);
    raw_events.free_byte += ntohl(event->len) + 4;
    compute_control_sum(raw_events);
}

/* Function that saves ip address and port number into client_data.
 */
static void get_client_socket(client_data_t &client, char *host, char *service) {
    strcpy(client.client_id.ip_address, host);
    client.client_id.port_number = atoi(service);
}

/* Function that changes byte order in client_message.
 */
static void repair_client_data(client_message_t &client_message, ssize_t len) {
    client_message.next_expected_event_no = ntohl(client_message.next_expected_event_no);
    client_message.session_id = be64toh(client_message.session_id);
    void *data = (void*) &client_message;
    ((char*)data)[len] = '\0';
}

/* Function that check if two clients come from same socket.
 */
static bool same_clients(const client_data_t &client1, const client_data_t &client2) {
    return client1.client_id.port_number == client2.client_id.port_number &&
            strcmp(client1.client_id.ip_address, client2.client_id.ip_address) == 0;
}

/* Function that creates or returns already existing client.
 * If occurred problem with client, nullptr is returned instead.
 * Additional, if client is not observer, pointer to player in client is not NULL.
 */
static client_data_t* create_client(client_message_t &client_message, clients_t &clients,
                             ssize_t len, char *host, char *service) {
    repair_client_data(client_message, len);
    if (client_message.turn_direction > 2 || !check_player_name(client_message.player_name)) {
        return nullptr;
    }
    client_data_t new_client;
    new_client.player = nullptr;
    get_client_socket(new_client, host, service);
    new_client.client_id.session_id = client_message.session_id;
    client_data_t* old_client = nullptr;
    for (auto &client : clients) {
        if (same_clients(new_client, client)) {
            old_client = &client;
            break;
        }
    }

    if (clients.size() == MAX_PLAYER && old_client == nullptr) {
        return nullptr;
    }

    if (old_client == nullptr) {
        clients.push_back(new_client);
        return &clients.back();
    }
    if (new_client.client_id.session_id < old_client->client_id.session_id) {
        return nullptr;
    }
    if (new_client.client_id.session_id == old_client->client_id.session_id) {
        if (old_client->player &&
            strcmp(old_client->player->player_name, client_message.player_name) != 0) {

            return nullptr;
        }
        return old_client;
    }
    if (old_client->player) {
        old_client->player->connected = false;
    }
    std::swap(*old_client, clients.back());
    clients.pop_back();
    clients.push_back(new_client);
    return &clients.back();
}

/* Function that creates new player.
 */
static void create_player(players_t &players, client_data_t &client,
                          client_message_t &client_message, bool eliminated) {

    auto new_player = new player_data_t;
    new_player->last_direction = client_message.turn_direction;
    strcpy(new_player->player_name, client_message.player_name);
    new_player->connected = true;
    new_player->eliminated = eliminated;
    client.player = new_player;
    players.player_data.push_back(new_player);
}

/* Function that clears raw_events before new_game.
 */
static void raw_events_clear(raw_events_t &raw_events) {
    raw_events.free_byte = 4;
    raw_events.events_no = 0;
    raw_events.raw_events_data.clear();
    raw_events.raw_events_start.clear();
}

/* Function that removes all inactive players.
 */
static void remove_inactive(players_t &players, clients_t &clients, bool remove_players) {
    auto now = std::chrono::steady_clock::now();
    for (int8 i = 0; i < (int8)clients.size(); i++) {
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - clients[i].last_activity);
        if (diff > std::chrono::milliseconds(TIMEOUT)) {
            if (clients[i].player != nullptr) {
                clients[i].player->connected = false;
            }
            std::swap(clients[i], clients.back());
            clients.pop_back();
            i--;
        }
    }
    if (remove_players) {
        for (int8 i = 0; i < (int8)players.player_data.size(); i++) {
            if (!players.player_data[i]->connected) {
                delete players.player_data[i];
                std::swap(players.player_data[i], players.player_data.back());
                players.player_data.pop_back();
            }
        }
    }
}

/* Function used to sort players lexicographically.
 */
bool comparator(const player_data_t *player1, const player_data_t *player2) {
    return strcmp(player1->player_name, player2->player_name) < 0;
}

/* Function that checks if all players are ready to play.
 */
static bool ready_declared(players_t &players) {
    sort(players.player_data.begin(), players.player_data.end(), comparator);
    if (players.player_data.size() < 2) {
        return false;
    }
    for (auto player : players.player_data) {
        if (player->last_direction == 0) {
            return false;
        }
    }
    return true;
}

/* Function that tests if game should start.
 */
static bool game_ready(game_data_t &game_data, players_t &players, clients_t &clients) {
    if (game_data.is_started || game_data.is_ready) {
        return false;
    }
    remove_inactive(players, clients, true);
    return ready_declared(players);
}

/* Function that handles all the game.
 */
static void game_handler(raw_events_t &raw_events, game_data_t &game_data) {
    if (game_data.is_started) {
        if (game_data.players->alive_players > 1) {
            for (int8 i = 0; i < (int8)game_data.players->player_data.size(); i++) {
                player_data_t *player_data = game_data.players->player_data[i];
                if (player_data->eliminated) {
                    continue;
                }
                if (player_data->last_direction == 1) {
                    player_data->direction += game_data.server_flags->turning_speed;
                } else if (player_data->last_direction == 2) {
                    player_data->direction -= game_data.server_flags->turning_speed;
                }
                pixel_t old_pixel = get_pixel(player_data->cord);
                player_data->cord.x += std::cos((double)player_data->direction * M_PI / 180.0);
                player_data->cord.y += std::sin((double)player_data->direction * M_PI / 180.0);
                pixel_t new_pixel = get_pixel(player_data->cord);
                if (new_pixel == old_pixel) {
                    continue;
                }
                if (get_owner(new_pixel, game_data)) {
                    player_data->eliminated = true;
                    game_data.players->alive_players--;
                    create_player_eliminated_event(player_data->player_no, raw_events);
                    if (game_data.players->alive_players == 1) {
                        break;
                    }
                }
                else {
                    game_data.board_data[new_pixel] = player_data;
                    create_pixel_event(player_data->player_no, new_pixel, raw_events);
                }
            }
        }
        else {
            create_game_over_event(raw_events);
            game_data.is_started = game_data.is_ready = false;
            game_data.board_data.clear();
        }
    }
    else if (game_data.is_ready) {
        raw_events_clear(raw_events);
        create_new_game_event(game_data, raw_events);
        game_data.players->alive_players = 0;
        for (int8 i = 0; i < (int8)game_data.players->player_data.size(); i++) {
            player_data_t *player = game_data.players->player_data[i];
            if (player->player_name[0] == '\0') {
                continue;
            }
            game_data.players->alive_players++;
            player->cord.x = (double)(custom_rand() % game_data.server_flags->width) + 0.5;
            player->cord.y = (double)(custom_rand() % game_data.server_flags->height) + 0.5;
            player->direction = custom_rand() % 360;
            player->player_no = i;
            player->eliminated = false;
            player->last_direction = 0;
        }
        game_data.is_started = true;
        game_data.game_id = custom_rand();
    }
}

/* Function gets and sets all user options.
 */
static void get_flags(int argc, char **argv, server_flags_t &server_flags) {
    while (optind < argc) {
        int8 c = getopt(argc, argv, PROGRAM_FLAGS);
        bool done = false;
        switch (c) {
            case 'p': {
                char *ptr;
                long int port = std::strtol(optarg, &ptr, 10);
                if (*ptr != '\0' || errno != 0 || port < 1 || MAX_PORT_NUMBER < port) {
                    fatal(INVALID_SERVER_PORT);
                }
                server_flags.port_number = port;
                break;
            }
            case 's': {
                char *ptr;
                long int seed = std::strtol(optarg, &ptr, 10);
                if (seed < 1 || errno != 0 || *ptr != '\0') {
                    fatal("invalid seed");
                }
                custom_rand_init(seed);
                break;
            }
            case 't': {
                char *ptr;
                long int speed = std::strtol(optarg, &ptr, 10);
                if (speed < 1 || speed >= 360 || errno != 0 || *ptr != '\0') {
                    fatal("invalid turning speed");
                }
                server_flags.turning_speed = speed;
                break;
            }
            case 'v': {
                char *ptr;
                long int speed = std::strtol(optarg, &ptr, 10);
                if (*ptr != '\0' || errno != 0 || speed < 1) {
                    fatal("invalid game speed");
                }
                server_flags.rounds_per_sec = speed;
                break;
            }
            case 'w': {
                char *ptr;
                long int width = std::strtol(optarg, &ptr, 10);
                if (*ptr != '\0' || errno != 0 || width < 1 || width > MAX_WIDTH) {
                    fatal("invalid game width");
                }
                server_flags.width = width;
                break;
            }
            case 'h': {
                char *ptr;
                long int height = std::strtol(optarg, &ptr, 10);
                if (*ptr != '\0' || errno != 0 || height < 1 || height > MAX_HEIGHT) {
                    fatal("invalid game height");
                }
                server_flags.height = height;
                break;
            }
            case 255: {
                fatal(USAGE);
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

int main(int argc, char **argv) {
    server_flags_t server_flags;
    get_flags(argc, argv, server_flags);
    raw_events_t raw_events;
    players_t players;
    game_data_t game_data;
    game_data.server_flags = &server_flags;
    game_data.players = &players;
    clients_t clients;
    write_work_t write_work;

    addrinfo hints;
    struct addrinfo *result, *rp;
    int sfd, s;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    s = getaddrinfo(nullptr,
                    std::to_string(server_flags.port_number).c_str(),
                    &hints, &result);

    if (s != 0) {
        syserr("cannot receive ip address");
    }
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            continue;
        }
        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sfd);
    }
    freeaddrinfo(result);
    if (rp == nullptr) {
        syserr("cannot find fitting ip address");
    }
    RaiiDescriptor sock{sfd};


    int descriptor = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (descriptor < 0) {
        syserr("cannot create timer");
    }
    RaiiDescriptor timer{descriptor};
    itimerspec expiration;
    expiration.it_value.tv_sec = 1;
    expiration.it_value.tv_nsec = 1;

    if (server_flags.rounds_per_sec > 1) {
        expiration.it_interval.tv_nsec = NS_IN_S / server_flags.rounds_per_sec;
        expiration.it_interval.tv_sec = 0;
    }
    else {
        expiration.it_interval.tv_nsec = 0;
        expiration.it_interval.tv_sec = 1;
    }

    if (timerfd_settime(descriptor, 0, &expiration, nullptr) < 0) {
        syserr("cannot arm timer");
    }

    descriptor = epoll_create(1);
    if (descriptor < 0) {
        syserr("cannot create epoll");
    }
    RaiiDescriptor epoll{descriptor};

    epoll_event timer_event;
    timer_event.data.fd = timer.descriptor;
    timer_event.events = EPOLLIN | EPOLLERR;
    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_ADD, timer.descriptor, &timer_event) < 0) {
        syserr("cannot add to epoll");
    }

    epoll_event sock_event;
    sock_event.data.fd = sock.descriptor;
    sock_event.events = EPOLLIN | EPOLLERR;
    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_ADD, sock.descriptor, &sock_event) < 0) {
        syserr("cannot add to epoll");
    }

    epoll_event catch_event[2];
    char host[NI_MAXHOST], service[NI_MAXSERV];

    while (true) {
        memset(catch_event, 0, 2 * sizeof(epoll_event));
        int desc = epoll_wait(epoll.descriptor, catch_event, 2,
                              TIMEOUT);

        if (desc == 0) {
            syserr("timer not responds");
        }
        if (desc < 0) {
            syserr("epoll not responds");
        }


        if (game_ready(game_data, players, clients)) {
            game_data.is_ready = true;
        }
        int8 ind = 0;
        if (catch_event[1].data.fd == timer.descriptor) {
            ind = 1;
        }
        int64 data;
        if (catch_event[ind].data.fd == timer.descriptor) {
            if (catch_event[ind].events & EPOLLERR) {
                syserr("timer error");
            }
            if (read(timer.descriptor, &data, 8) < 0) {
                continue;
            }
            game_handler(raw_events, game_data);
        }
        ind = 0;
        if (catch_event[1].data.fd == sock.descriptor) {
            ind = 1;
        }

        if (catch_event[ind].data.fd == sock.descriptor) {
            if (catch_event[ind].events & EPOLLERR) {
                continue;
            }
            if (catch_event[ind].events & EPOLLIN) {
                sockaddr_storage peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);
                client_message_t client_message;
                memset(&client_message, 0, sizeof(client_message_t));
                write_data_t write_data;
                memset(&write_data, 0, sizeof(write_data_t));
                memset(&peer_addr, 0, sizeof(sockaddr_storage));
                ssize_t len = recvfrom(sock.descriptor, &client_message, sizeof(client_message_t),
                                       0, (sockaddr*) &peer_addr, &peer_addr_len);
                write_data.client_len = peer_addr_len;
                void *data2 = (void*)&peer_addr;
                write_data.client_addr = *(sockaddr*)data2;

                s = getnameinfo((struct sockaddr *) &peer_addr,
                                peer_addr_len, host, NI_MAXHOST,
                                service, NI_MAXSERV, NI_NUMERICSERV);

                if (s != 0) {
                    syserr("cannot resolve client name");
                }


                client_data_t *new_client;
                if (len >= MIN_CLIENT_MESSAGE_LEN && len < MAX_CLIENT_MESSAGE_LEN && s == 0 &&
                    (new_client = create_client(client_message, clients,
                                                len, host, service))) {

                    new_client->last_activity = std::chrono::steady_clock::now();
                    if (client_message.player_name[0] != '\0'
                        && new_client->player == nullptr) {

                        create_player(players, *new_client, client_message, game_data.is_started);
                    }
                    player_data_t *player = new_client->player;
                    if (player != nullptr) {
                        player->last_direction = client_message.turn_direction;
                    }
                    write_data.begin_event = client_message.next_expected_event_no;
                    write_work.push_back(write_data);
                    sock_event.events |= EPOLLOUT;
                    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_MOD, sock.descriptor, &sock_event) < 0) {
                        syserr("cannot modify epoll");
                    }
                }
            }
            if (catch_event[ind].events & EPOLLOUT) {
                write_data_t work = write_work.front();
                write_work.pop_front();
                if (work.begin_event < raw_events.events_no) {
                    int32 begin_addr = raw_events.raw_events_start[work.begin_event];
                    int32 last_event = work.begin_event;
                    int32 len = raw_events.free_byte - begin_addr;
                    if (work.begin_event + 1 < raw_events.events_no) {
                        len = raw_events.raw_events_start[work.begin_event + 1] - begin_addr;
                    }
                    while (last_event + 2 < raw_events.events_no &&
                           len + raw_events.raw_events_start[last_event + 2] -
                           raw_events.raw_events_start[last_event + 1] <= MAX_DATAGRAM - 4) {


                        len += raw_events.raw_events_start[last_event + 2] -
                               raw_events.raw_events_start[last_event + 1];
                        last_event++;
                    }
                    if (last_event + 2 == raw_events.events_no &&
                        len + raw_events.free_byte - raw_events.raw_events_start[last_event + 1] <=
                        MAX_DATAGRAM - 4) {

                        len += raw_events.free_byte - raw_events.raw_events_start[last_event + 1];
                        last_event++;
                    }
                    len += 4;
                    int32 *ptr_to_begin = (int32 *) (&raw_events.raw_events_data[begin_addr] - 4);
                    int32 save_bytes = *ptr_to_begin;
                    *ptr_to_begin = htonl(game_data.game_id);

                    ssize_t err;
                    if ((err = sendto(sock.descriptor,
                                      (&raw_events.raw_events_data[begin_addr]) - 4, len, 0,
                                            &work.client_addr, work.client_len)) == len &&
                        last_event + 1 < raw_events.events_no) {

                        write_work.push_back(work);
                        work.begin_event = last_event + 1;

                    }
                    if (err < 0) {
                        continue;
                    }
                    *ptr_to_begin = save_bytes;
                }

                if (write_work.empty()) {
                    sock_event.events ^= EPOLLOUT;
                    if (epoll_ctl(epoll.descriptor, EPOLL_CTL_MOD, sock.descriptor, &sock_event) < 0) {
                        syserr("cannot modify epoll error");
                    }
                }
            }
        }
    }
}