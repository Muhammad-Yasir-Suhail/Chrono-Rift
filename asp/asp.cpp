// Chrono Rift — Automated Strategic Process (ASP)
#include "ipc.h"
#include <csignal>
#include <pthread.h>
#include <iostream>
#include <cstdlib>
// ============================================================
// Globals
// ============================================================
static volatile int               g_keep_running = 1;
static chrono_rift::SharedState*  g_state        = nullptr;
static int                        g_fd           = -1;
// ============================================================
// signal_thread_fn
// ============================================================
static void* signal_thread_fn(void*) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    int sig = 0;
    while (sigwait(&set, &sig) == 0) {
        if (sig == SIGINT || sig == SIGTERM) {
            g_keep_running = 0;
            if (g_state) {
                sem_wait(&g_state->state_lock);
                g_state->running = 0;
                sem_post(&g_state->state_lock);
                for (int i = 0; i < chrono_rift::kMaxEnemies; ++i) {
                    sem_post(&g_state->enemy_stun_sem[i]);
                }
            }
            break;
        } else if (sig == SIGUSR1) {
            std::cout << "[asp] SIGUSR1 received: stun event delivered\n";
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
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    pthread_t t;
    pthread_create(&t, nullptr, signal_thread_fn, nullptr);
    pthread_detach(t);
}
// ============================================================
// EnemyArg
// ============================================================
struct EnemyArg { int idx; };
// ============================================================
// enemy_thread_fn
// ============================================================
static void* enemy_thread_fn(void* arg) {
    int idx = static_cast<EnemyArg*>(arg)->idx;
    int turns_taken = 0;
    std::cout << "[asp] Enemy " << idx << " thread started\n";
    while (g_keep_running) {
        if (!g_state || !g_state->running) {
            break;
        }
        sem_wait(&g_state->state_lock);
        int running   = g_state->running;
        int alive     = (idx < g_state->active_enemies) ? g_state->enemies[idx].alive : 0;
        int stunned   = g_state->enemy_stunned[idx];
        int turn_kind = g_state->turn_actor_kind;
        int turn_idx  = g_state->turn_actor_index;
        int buf_ready = g_state->move_buffer.ready;
        sem_post(&g_state->state_lock);
        if (!running) {
            break;
        }
        if (!alive) {
            std::cout << "[asp] Enemy " << idx << " thread exiting (dead, "
                      << turns_taken << " turns taken)\n";
            return nullptr;
        }
        if (stunned) {
            std::cout << "[asp] Enemy " << idx << " blocked on stun semaphore\n";
            sem_wait(&g_state->enemy_stun_sem[idx]);
            std::cout << "[asp] Enemy " << idx << " stun over, resuming\n";
            continue;
        }
        if (turn_kind == 1 && turn_idx == idx && buf_ready == 0) {
            sem_wait(&g_state->state_lock);
            if (g_state->turn_actor_kind  == 1 &&
                g_state->turn_actor_index == idx &&
                g_state->move_buffer.ready == 0 &&
                g_state->enemies[idx].alive &&
                !g_state->enemy_stunned[idx]) {
                int roll = rand() % 10;
                if (roll < 7) {
                    g_state->move_buffer.action_type  = chrono_rift::kActionStrike;
                    g_state->move_buffer.target_index = -1;
                    g_state->move_buffer.damage       = g_state->enemies[idx].damage;
                } else if (roll < 9) {
                    g_state->move_buffer.action_type  = chrono_rift::kActionSkip;
                    g_state->move_buffer.target_index = -1;
                    g_state->move_buffer.damage       = 0;
                } else {
                    g_state->move_buffer.action_type  = chrono_rift::kActionLock;
                    g_state->move_buffer.target_index = rand() % chrono_rift::kNumArtifacts;
                    g_state->move_buffer.damage       = 0;
                }
                g_state->move_buffer.ready       = 1;
                g_state->move_buffer.actor_kind  = 1;
                g_state->move_buffer.actor_index = idx;
                ++turns_taken;
            }
            sem_post(&g_state->state_lock);
        } else {
            usleep(100000);
        }
    }
    std::cout << "[asp] Enemy " << idx << " thread exiting (game ended, "
              << turns_taken << " turns taken)\n";
    return nullptr;
}
// ============================================================
// main
// ============================================================
int main() {
    install_signals();
    g_state = chrono_rift::attach_shared_state(g_fd);
    sem_wait(&g_state->state_lock);
    g_state->asp_pid = getpid();
    chrono_rift::copy_text(g_state->asp_status, chrono_rift::kStatusSize, "asp attached");
    sem_post(&g_state->state_lock);
    sem_post(&g_state->asp_ready);
    if (!chrono_rift::sem_wait_interruptible(&g_state->arbiter_ready, (int&)g_keep_running)) {
        chrono_rift::detach_shared_state(g_state, g_fd);
        return EXIT_FAILURE;
    }
    std::cout << "[asp] connected to shared memory (pid=" << getpid() << ")\n";
    while (g_keep_running && g_state->running) {
        sem_wait(&g_state->state_lock);
        int ne = g_state->active_enemies;
        sem_post(&g_state->state_lock);
        if (ne > 0) {
            break;
        }
        usleep(100000);
    }
    pthread_t  threads[chrono_rift::kMaxEnemies];
    EnemyArg   args[chrono_rift::kMaxEnemies];
    int        spawned = 0;
    while (g_keep_running && g_state->running) {
        sem_wait(&g_state->state_lock);
        int active  = g_state->running ? g_state->active_enemies : 0;
        sem_post(&g_state->state_lock);
        if (!g_state->running) {
            break;
        }
        if (spawned > 0) {
            bool all_dead = true;
            for (int i = 0; i < spawned; ++i) {
                sem_wait(&g_state->state_lock);
                int alive_i = g_state->enemies[i].alive;
                sem_post(&g_state->state_lock);
                if (alive_i) {
                    all_dead = false; break;
                }
            }
            if (all_dead) {
                for (int i = 0; i < spawned; ++i) {
                    pthread_join(threads[i], nullptr);
                }
                std::cout << "[asp] Wave cleared, " << spawned
                          << " threads joined, ready for next wave\n";
                spawned = 0;
            }
        }
        if (active > spawned) {
            for (int i = spawned; i < active && i < chrono_rift::kMaxEnemies; ++i) {
                args[i].idx = i;
                pthread_create(&threads[i], nullptr, enemy_thread_fn, &args[i]);
                std::cout << "[asp] Enemy " << i << " thread spawned\n";
            }
            spawned = active;
        }
        usleep(200000);
    }
    sem_wait(&g_state->state_lock);
    int winner = g_state->winner;
    sem_post(&g_state->state_lock);
    if (winner == 0) {
        std::cout << "[asp] GAME OVER: Players win!\n";
    } else if (winner == 1) {
        std::cout << "[asp] GAME OVER: Enemies win!\n";
    }
    for (int i = 0; i < chrono_rift::kMaxEnemies; ++i) {
        sem_post(&g_state->enemy_stun_sem[i]);
    }
    for (int i = 0; i < spawned; ++i) {
        pthread_join(threads[i], nullptr);
    }
    chrono_rift::detach_shared_state(g_state, g_fd);
    std::cout << "[asp] shutdown complete\n";
    return EXIT_SUCCESS;
}