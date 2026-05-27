// Human Interfacing Process (HIP)
#include "ipc.h"
#include <csignal>
#include <pthread.h>
#include <poll.h>
#include <iostream>
#include <sstream>
// ============================================================
// Globals
// ============================================================
static volatile int              g_keep_running = 1;
static chrono_rift::SharedState* g_state        = nullptr;
static int                       g_fd           = -1;
// ============================================================
// signal_thread_fn
// ============================================================
static void* signal_thread_fn(void*) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int sig = 0;
    while (sigwait(&set, &sig) == 0) {
        if (sig == SIGINT || sig == SIGTERM) {
            g_keep_running = 0;
            if (g_state && g_state->arbiter_pid > 0) {
                std::cout << "[hip] Quit requested — sending SIGTERM to Arbiter\n";
                kill(g_state->arbiter_pid, SIGTERM);
            }
            if (g_state) {
                sem_wait(&g_state->state_lock);
                g_state->running = 0;
                sem_post(&g_state->state_lock);
            }
            break;
        }
    }
    return nullptr;
}
// ============================================================
// install_signals
// ============================================================
static void install_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    pthread_t t;
    pthread_create(&t, nullptr, signal_thread_fn, nullptr);
    pthread_detach(t);
}
// ============================================================
// read_console_line
// ============================================================
static bool read_console_line(int& action_type, int& target_index) {
    struct pollfd pfd{};
    pfd.fd     = STDIN_FILENO;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    std::istringstream ss(line);
    if (!(ss >> action_type)) {
        return false;
    }
    if (!(ss >> target_index)) {
        target_index = -1;
    }
    return true;
}
// ============================================================
// submit_action
// ============================================================
static bool submit_action(int player_idx, int action_type, int target_index) {
    sem_wait(&g_state->state_lock);
    bool ok = (g_state->turn_actor_kind  == 0 &&
               g_state->turn_actor_index == player_idx &&
               g_state->move_buffer.ready == 0);
    if (ok) {
        g_state->move_buffer.ready        = 1;
        g_state->move_buffer.actor_kind   = 0;
        g_state->move_buffer.actor_index  = player_idx;
        g_state->move_buffer.action_type  = action_type;
        g_state->move_buffer.target_index = target_index;
        g_state->move_buffer.damage       = g_state->players[player_idx].damage;
    }
    sem_post(&g_state->state_lock);
    return ok;
}
// ============================================================
// PlayerArg
// ============================================================
struct PlayerArg { int index; };
// ============================================================
// player_thread_fn
// ============================================================
static void* player_thread_fn(void* arg) {
    int idx = static_cast<PlayerArg*>(arg)->index;
    bool prompted = false;
    while (g_keep_running) {
        sem_wait(&g_state->state_lock);
        int running   = g_state->running;
        int tk        = g_state->turn_actor_kind;
        int ti        = g_state->turn_actor_index;
        int buf_ready = g_state->move_buffer.ready;
        sem_post(&g_state->state_lock);
        if (!running) {
            break;
        }
        if (tk == 0 && ti == idx && buf_ready == 0) {
            if (!prompted) {
                int pickup_idx = -1;
                const char* pickup_label = "none";
                sem_wait(&g_state->state_lock);
                pickup_idx = g_state->pending_drop_weapon;
                if (pickup_idx >= 0 && pickup_idx < chrono_rift::kNumWeapons) {
                    pickup_label = g_state->weapons[pickup_idx].name;
                }
                sem_post(&g_state->state_lock);
                std::cout << "\n[hip] === Player " << idx << " Turn ===\n"
                          << "  Pickup available: " << pickup_label;
                if (pickup_idx >= 0) {
                    std::cout << " (" << g_state->weapons[pickup_idx].slots << " slots, action 5 "
                              << pickup_idx << ")";
                }
                std::cout << "\n"
                          << "  Actions:\n"
                          << "   1        = Strike (attack enemy HP)\n"
                          << "   8        = Exhaust (attack enemy Stamina)\n"
                          << "   9        = Heal (restore 10% own HP)\n"
                          << "  10 <t>   = Use Weapon on enemy <t>\n"
                          << "   2        = Skip turn\n"
                          << "   3        = Ultimate (need Solar Core + Lunar Blade)\n"
                          << "   4 <t>   = Stun enemy <t>\n"
                          << "   5 <w>   = Pickup weapon <w> (0=Solar,1=Lunar,2=Halberd,3=Venom,4=Thunder,5=Obsidian,6=Frost,7=Splinter)\n"
                          << "  11 <w>   = Swap In weapon <w> from long-term storage\n"
                          << "   6 <a>   = Lock artifact <a>\n"
                          << "   7        = Unlock all artifacts\n"
                          << "  Enter action [action_type] [optional_target]: " << std::flush;
                prompted = true;
            }
            int at = -1, tgt = -1;
            if (read_console_line(at, tgt)) {
                if (submit_action(idx, at, tgt)) {
                    std::cout << "[hip] Player " << idx << " submitted action "
                              << at << " target " << tgt << "\n";
                    prompted = false;
                }
            } else {
                usleep(100000);
            }
        } else {
            prompted = false;
            usleep(100000);
        }
    }
    std::cout << "[hip] Player " << idx << " thread exiting\n";
    return nullptr;
}
// ============================================================
// main
// ============================================================
int main() {
    install_signals();
    g_state = chrono_rift::attach_shared_state(g_fd);
    sem_wait(&g_state->state_lock);
    g_state->hip_pid = getpid();
    chrono_rift::copy_text(g_state->hip_status, chrono_rift::kStatusSize, "hip attached");
    sem_post(&g_state->state_lock);
    sem_post(&g_state->hip_ready);
    if (!chrono_rift::sem_wait_interruptible(&g_state->arbiter_ready, (int&)g_keep_running)) {
        chrono_rift::detach_shared_state(g_state, g_fd);
        return EXIT_FAILURE;
    }
    std::cout << "[hip] connected to shared memory (pid=" << getpid() << ")\n";
    std::cout << "[hip] multi-threaded input active — one thread per player\n";
    int np = 0;
    while (g_keep_running && g_state->running) {
        sem_wait(&g_state->state_lock);
        np = g_state->active_players;
        sem_post(&g_state->state_lock);
        if (np > 0) {
            break;
        }
        usleep(100000);
    }
    pthread_t   threads[chrono_rift::kMaxPlayers];
    PlayerArg   args[chrono_rift::kMaxPlayers];
    for (int i = 0; i < np; ++i) {
        args[i].index = i;
        pthread_create(&threads[i], nullptr, player_thread_fn, &args[i]);
        std::cout << "[hip] Player " << i << " thread spawned\n";
    }
    while (g_keep_running && g_state->running) {
        usleep(200000);
    }
    sem_wait(&g_state->state_lock);
    int winner = g_state->winner;
    sem_post(&g_state->state_lock);
    if (winner == 0) {
        std::cout << "[hip] GAME OVER: Players win!\n";
    } else if (winner == 1) {
        std::cout << "[hip] GAME OVER: Enemies win!\n";
    } else {
        std::cout << "[hip] GAME OVER: Quit\n";
    }
    for (int i = 0; i < np; ++i) {
        pthread_join(threads[i], nullptr);
    }
    chrono_rift::detach_shared_state(g_state, g_fd);
    std::cout << "[hip] shutdown complete\n";
    return EXIT_SUCCESS;
}