// Roll Number: 25
//   Player HP   = 25 + rand(100..1000)   [roll_no + rand 100-1000]
//   Player DMG  = 5 + 10 = 15            [last digit=5, +10]
//   Enemy  HP   = 25 + rand(50..200)     [last 2 digits=25, +50-200]
//   Enemy  DMG  = 2 + 10 = 12            [second-last digit=2, +10]
//   srand seed  = 25
#include "ipc.h"
#include "renderer.h"
#include <csignal>
#include <pthread.h>
#include <iostream>
#include <string>
#include <ctime>
#include <sstream>
#include <cstdlib>
#include <cstdarg>
// ============================================================
// Global
// ============================================================
static volatile int           g_keep_running = 1;
static chrono_rift::SharedState* g_state    = nullptr;
static int                    g_fd          = -1;
static chrono_rift::GameRenderer* g_renderer = nullptr;
// ============================================================
// log_locked
// ============================================================
static void log_locked(chrono_rift::SharedState* s, const char* text) {
    if (!s) {
        return;
    }
    int slot = s->action_log_head % chrono_rift::kActionLogEntries;
    chrono_rift::copy_text(s->action_log[slot], chrono_rift::kStatusSize, text);
    s->action_log_head = (s->action_log_head + 1) % chrono_rift::kActionLogEntries;
    if (s->action_log_count < chrono_rift::kActionLogEntries) {
        ++s->action_log_count;
    }
}
static void log_fmt(chrono_rift::SharedState* s, const char* fmt, ...) {
    char buf[160];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    log_locked(s, buf);
    std::cout << "[arbiter] " << buf << "\n";
}
static bool enemy_take_ground_weapon(chrono_rift::SharedState* s, int wi);
// ============================================================
// launch_child_process
// ============================================================
static bool launch_child_process(const char* path, const char* label, pid_t& child_pid) {
    child_pid = fork();
    if (child_pid < 0) {
        std::perror("fork");
        return false;
    }
    if (child_pid == 0) {
        execl(path, label, static_cast<char*>(nullptr));
        std::perror(path);
        _exit(EXIT_FAILURE);
    }
    std::cout << "[arbiter] launched " << label << " (pid=" << child_pid << ")\n";
    return true;
}
// ============================================================
// signal_thread_fn
// ============================================================
static void* signal_thread_fn(void*) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGALRM);
    int sig = 0;
    while (1) {
        if (sigwait(&set, &sig) != 0) {
            continue;
        }
        if (sig == SIGINT || sig == SIGTERM) {
            g_keep_running = 0;
            if (g_state) {
                sem_wait(&g_state->state_lock);
                g_state->running = 0;
                sem_post(&g_state->state_lock);
                sem_post(&g_state->hip_ready);
                sem_post(&g_state->asp_ready);
                sem_post(&g_state->arbiter_ready);
                sem_post(&g_state->arbiter_ready);
            }
            break;
        } 
        else if (sig == SIGALRM) {
            if (g_state && g_state->asp_pid > 0) {
                kill(g_state->asp_pid, SIGCONT);
                sem_wait(&g_state->state_lock);
                log_locked(g_state, "Ultimate window ended — ASP resumed");
                sem_post(&g_state->state_lock);
                std::cout << "[arbiter] SIGALRM: ASP resumed after ultimate\n";
            }
        }
    }
    return nullptr;
}
static void install_signal_mask_and_thread() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    pthread_t t;
    pthread_create(&t, nullptr, signal_thread_fn, nullptr);
    pthread_detach(t);
}
// ============================================================
// render_thread_fn
// ============================================================
static void* render_thread_fn(void* arg) {
    chrono_rift::SharedState* s = static_cast<chrono_rift::SharedState*>(arg);
    try {
        chrono_rift::GameRenderer renderer(s);
        g_renderer = &renderer;
        std::cout << "[arbiter] render thread started\n";
        while (renderer.is_running() && g_keep_running) {
            renderer.update_and_render();
        }
        g_renderer = nullptr;
        std::cout << "[arbiter] render thread finished\n";
    } catch (const std::exception& e) {
        std::cout << "[arbiter] render error: " << e.what() << "\n";
        g_renderer = nullptr;
    }
    return nullptr;
}
// ============================================================
// detect_deadlock
// ============================================================
static bool detect_deadlock(chrono_rift::SharedState* s,
                             int& vk, int& vi) {
    for (int i = 0; i < chrono_rift::kNumArtifacts; ++i) {
        auto& ai = s->artifacts[i];
        if (!ai.locked || ai.waiting_kind == -1) {
            continue;
        }
        int wk = ai.waiting_kind, wi = ai.waiting_index;
        int ok = ai.lock_owner_kind, oi = ai.lock_owner_index;
        for (int j = 0; j < chrono_rift::kNumArtifacts; ++j) {
            if (i == j) {
                continue;
            }
            auto& aj = s->artifacts[j];
            if (!aj.locked || aj.waiting_kind == -1) {
                continue;
            }
            if (aj.waiting_kind == ok && aj.waiting_index == oi &&
                aj.lock_owner_kind == wk && aj.lock_owner_index == wi) {
                int hp_a = (ok == 0 && oi < s->active_players) ?
                            s->players[oi].hp : (ok == 1 && oi < s->active_enemies) ?
                            s->enemies[oi].hp : 0;
                int hp_w = (wk == 0 && wi < s->active_players) ?
                            s->players[wi].hp : (wk == 1 && wi < s->active_enemies) ?
                            s->enemies[wi].hp : 0;
                if (hp_a <= hp_w) {
                    vk = ok; vi = oi;
                } else {
                    vk = wk; vi = wi;
                }
                return true;
            }
        }
    }
    return false;
}
// ============================================================
// release_all_locks
// ============================================================
static void release_all_locks(chrono_rift::SharedState* s, int kind, int idx) {
    for (int i = 0; i < chrono_rift::kNumArtifacts; ++i) {
        auto& a = s->artifacts[i];
        if (a.lock_owner_kind == kind && a.lock_owner_index == idx) {
            if (a.waiting_kind != -1) {
                a.lock_owner_kind  = a.waiting_kind;
                a.lock_owner_index = a.waiting_index;
                a.waiting_kind     = -1;
                a.waiting_index    = -1;
            } else {
                a.locked           = 0;
                a.lock_owner_kind  = -1;
                a.lock_owner_index = -1;
            }
        }
        if (a.waiting_kind == kind && a.waiting_index == idx) {
            a.waiting_kind  = -1;
            a.waiting_index = -1;
        }
    }
}
// ============================================================
// deadlock_monitor_fn
// ============================================================
static void* deadlock_monitor_fn(void*) {
    while (g_keep_running) {
        usleep(600000);
        if (!g_state || !g_state->running) {
            continue;
        }
        sem_wait(&g_state->state_lock);
        int vk = -1, vi = -1;
        if (detect_deadlock(g_state, vk, vi)) {
            release_all_locks(g_state, vk, vi);
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Deadlock detected! Released locks held by %s %d",
                     vk == 0 ? "Player" : "Enemy", vi);
            log_locked(g_state, msg);
            std::cout << "[arbiter] " << msg << "\n";
        }
        static int pending_ticks = 0;
        if (g_state->pending_drop_weapon >= 0) {
            ++pending_ticks;
            if (pending_ticks >= 10) {
                int wi  = g_state->pending_drop_weapon;
                if (enemy_take_ground_weapon(g_state, wi)) {
                    g_state->pending_drop_weapon = -1;
                    g_state->pending_drop_enemy  = -1;
                    pending_ticks = 0;
                } else {
                    g_state->pending_drop_weapon = -1;
                    g_state->pending_drop_enemy  = -1;
                    pending_ticks = 0;
                }
            }
        } else {
            pending_ticks = 0;
        }
        sem_post(&g_state->state_lock);
    }
    return nullptr;
}
// ============================================================
// pickup_weapon
// ============================================================
static bool pickup_weapon(chrono_rift::SharedState* s, int player_idx, int wi) {
    (void)player_idx;
    int slots = s->weapons[wi].slots;
    if (chrono_rift::inventory_has(s->inventory_slots, wi)) {
        return true;
    }
    int start = chrono_rift::inventory_find_free(s->inventory_slots, slots);
    if (start != -1) {
        chrono_rift::inventory_place(s->inventory_slots, start, wi, slots);
        return true;
    }
    bool seen[chrono_rift::kNumWeapons] = {};
    int evict_order[chrono_rift::kNumWeapons];
    int ec = 0;
    for (int i = 0; i < chrono_rift::kInventorySlots; ++i) {
        int w = s->inventory_slots[i];
        if (w >= 0 && !seen[w]) {
            seen[w] = true; evict_order[ec++] = w;
        }
    }
    for (int i = 0; i < ec - 1; ++i) {
        for (int j = i + 1; j < ec; ++j) {
            if (s->weapons[evict_order[j]].slots < s->weapons[evict_order[i]].slots) {
                int t = evict_order[i]; evict_order[i] = evict_order[j]; evict_order[j] = t;
            }
        }
    }
    for (int k = 0; k < ec; ++k) {
        int victim = evict_order[k];
        if (victim == wi) {
            continue;
        }
        if (s->storage_count >= chrono_rift::kMaxStorage) {
            break;
        }
        chrono_rift::inventory_remove(s->inventory_slots, victim);
        chrono_rift::storage_add(s->storage, s->storage_count, victim);
        char msg[120];
        snprintf(msg, sizeof(msg), "%s swapped to storage to make room",
                 s->weapons[victim].name);
        log_locked(s, msg);
        std::cout << "[arbiter] " << msg << "\n";
        start = chrono_rift::inventory_find_free(s->inventory_slots, slots);
        if (start != -1) {
            chrono_rift::inventory_place(s->inventory_slots, start, wi, slots);
            return true;
        }
    }
    return false;
}
// ============================================================
// enemy_take_ground_weapon
// ============================================================
static bool enemy_take_ground_weapon(chrono_rift::SharedState* s, int wi) {
    if (wi < 0 || wi >= chrono_rift::kNumWeapons) {
        return false;
    }
    int chosen = -1;
    for (int i = 0; i < s->active_enemies; ++i) {
        if (s->enemies[i].alive) {
            chosen = i;
            if (s->enemy_weapons[i] < 0) {
                break;
            }
        }
    }
    if (chosen == -1) {
        return false;
    }
    s->enemy_weapons[chosen] = wi;
    char buf[120];
    snprintf(buf, sizeof(buf), "Enemy %d auto-picked up %s",
             chosen, s->weapons[wi].name);
    log_locked(s, buf);
    std::cout << "[arbiter] " << buf << "\n";
    return true;
}
// ============================================================
// swap_in_weapon
// ============================================================
static bool swap_in_weapon(chrono_rift::SharedState* s, int player_idx, int wi) {
    bool in_storage = false;
    for (int i = 0; i < s->storage_count; ++i) {
        if (s->storage[i].weapon_index == wi) {
            in_storage = true; break;
        }
    }
    if (!in_storage) {
        return false;
    }
    chrono_rift::storage_remove(s->storage, s->storage_count, wi);
    if (pickup_weapon(s, player_idx, wi)) {
        return true;
    }
    chrono_rift::storage_add(s->storage, s->storage_count, wi);
    return false;
}
// ============================================================
// maybe_spawn_eclipse
// ============================================================
static void maybe_spawn_eclipse(chrono_rift::SharedState* s) {
    if (s->enemies_killed < 3) {
        return;
    }
    auto& ec = s->artifacts[chrono_rift::kArtifactEclipse];
    if (ec.held_by_kind != -2) {
        return;
    }
    chrono_rift::copy_text(ec.name, chrono_rift::kNameSize, "Eclipse Relic");
    ec.held_by_kind    = -1;
    ec.held_by_index   = -1;
    ec.waiting_kind    = -1;
    ec.waiting_index   = -1;
    ec.locked          = 0;
    ec.lock_owner_kind = -1;
    ec.lock_owner_index= -1;
    log_locked(s, "Eclipse Relic has appeared in the arena!");
    std::cout << "[arbiter] Eclipse Relic spawned\n";
}
// ============================================================
// ultimate_available
// ============================================================
static bool ultimate_available(chrono_rift::SharedState* s) {
    return chrono_rift::inventory_has(s->inventory_slots, chrono_rift::kWeaponSolarCore) &&
           chrono_rift::inventory_has(s->inventory_slots, chrono_rift::kWeaponLunarBlade);
}
// ============================================================
// handle_artifact_lock
// ============================================================
static void handle_artifact_lock(chrono_rift::SharedState* s,
                                  int actor_kind, int actor_index,
                                  int artifact_idx) {
    if (artifact_idx < 0 || artifact_idx >= chrono_rift::kNumArtifacts) {
        for (int i = 0; i < chrono_rift::kNumArtifacts; ++i) {
            if (s->artifacts[i].name[0] != '\0') {
                artifact_idx = i; break;
            }
        }
    }
    if (artifact_idx < 0 || artifact_idx >= chrono_rift::kNumArtifacts) {
        return;
    }
    auto& a = s->artifacts[artifact_idx];
    if (a.name[0] == '\0') {
        return;
    }
    char msg[160];
    if (!a.locked || a.lock_owner_kind == -1) {
        a.locked = 1;
        a.lock_owner_kind  = actor_kind;
        a.lock_owner_index = actor_index;
        a.waiting_kind     = -1;
        a.waiting_index    = -1;
        snprintf(msg, sizeof(msg), "%s %d locked %s",
                 actor_kind==0?"Player":"Enemy", actor_index, a.name);
    } else if (a.lock_owner_kind == actor_kind && a.lock_owner_index == actor_index) {
        snprintf(msg, sizeof(msg), "%s %d already owns %s",
                 actor_kind==0?"Player":"Enemy", actor_index, a.name);
    } else {
        a.waiting_kind  = actor_kind;
        a.waiting_index = actor_index;
        snprintf(msg, sizeof(msg), "%s %d waiting for %s (held by %s %d)",
                 actor_kind==0?"Player":"Enemy", actor_index, a.name,
                 a.lock_owner_kind==0?"Player":"Enemy", a.lock_owner_index);
    }
    log_locked(s, msg);
    std::cout << "[arbiter] " << msg << "\n";
}
// ============================================================
// select_party_size
// ============================================================
static int select_party_size() {
    sf::RenderWindow win(sf::VideoMode(760, 420),
                         "Chrono Rift - Select Party Size",
                         sf::Style::Titlebar | sf::Style::Close);
    win.setFramerateLimit(60);
    sf::Font font;
    bool fok = font.loadFromFile("assets/fonts/PressStart2P-Regular.ttf");
    auto draw_txt = [&](const std::string& t, float x, float y,
                        unsigned sz, sf::Color c) {
        if (!fok) {
            return;
        }
        sf::Text tx(t, font, sz);
        tx.setPosition(x, y);
        tx.setFillColor(c);
        win.draw(tx);
    };
    int hovered = -1;
    while (win.isOpen()) {
        sf::Event ev;
        while (win.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                win.close(); return 2;
            }
            if (ev.type == sf::Event::KeyPressed) {
                if (ev.key.code >= sf::Keyboard::Num1 && ev.key.code <= sf::Keyboard::Num4) {
                    return (ev.key.code - sf::Keyboard::Num1) + 1;
                }
                if (ev.key.code == sf::Keyboard::Escape) {
                    win.close(); return 2;
                }
            }
            if (ev.type == sf::Event::MouseMoved) {
                hovered = -1;
                for (int i = 0; i < 4; ++i) {
                    float bx = 80.f + i * 165.f, by = 180.f;
                    if (ev.mouseMove.x>=bx && ev.mouseMove.x<=bx+140.f &&
                        ev.mouseMove.y>=by && ev.mouseMove.y<=by+110.f) {
                        hovered = i + 1;
                    }
                }
            }
            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {
                for (int i = 0; i < 4; ++i) {
                    float bx = 80.f + i * 165.f, by = 180.f;
                    if (ev.mouseButton.x>=bx && ev.mouseButton.x<=bx+140.f &&
                        ev.mouseButton.y>=by && ev.mouseButton.y<=by+110.f) {
                        return i + 1;
                    }
                }
            }
        }
        win.clear(sf::Color(17, 22, 33));
        sf::RectangleShape card(sf::Vector2f(700.f, 340.f));
        card.setPosition(30.f, 40.f);
        card.setFillColor(sf::Color(23, 30, 46));
        card.setOutlineThickness(2.f);
        card.setOutlineColor(sf::Color(75, 125, 210));
        win.draw(card);
        draw_txt("CHRONO RIFT",      250.f, 70.f,  22, sf::Color(255, 210, 90));
        draw_txt("Select party size", 195.f, 120.f, 14, sf::Color(210, 220, 235));
        for (int i = 0; i < 4; ++i) {
            float bx = 80.f + i * 165.f, by = 180.f;
            bool hov = (hovered == i+1);
            sf::RectangleShape btn(sf::Vector2f(140.f, 110.f));
            btn.setPosition(bx, by);
            btn.setFillColor(hov ? sf::Color(86,145,230) : sf::Color(48,76,122));
            btn.setOutlineThickness(2.f);
            btn.setOutlineColor(hov ? sf::Color(245,250,255) : sf::Color(130,165,220));
            win.draw(btn);
            draw_txt(std::to_string(i+1), bx+55.f, by+28.f, 28, sf::Color::White);
            draw_txt("PLAYER",           bx+30.f, by+75.f,  10, sf::Color(235,240,250));
        }
        draw_txt("Press 1-4 for quick selection", 140.f, 330.f, 10, sf::Color(170,195,230));
        win.display();
    }
    return 2;
}
// ============================================================
// init_entities
// ============================================================
static void init_entities(chrono_rift::SharedState* s, int np) {
    const int ROLL_NO        = 25;
    const int LAST_DIGIT     = 5;
    const int SECOND_LAST    = 2;
    const int LAST_TWO       = 25;
    s->active_players = np;
    s->enemies_killed = 0;
    for (int i = 0; i < np; ++i) {
        auto& p     = s->players[i];
        p.alive       = 1;
        p.max_hp      = ROLL_NO + 100 + (rand() % 901);
        p.hp          = p.max_hp;
        p.damage      = LAST_DIGIT + 10;
        p.speed       = 100 / np;
        p.max_stamina = 100;
        p.stamina     = 0;
        p.stunned     = 0;
        p.actor_id    = i;
        char nm[32]; snprintf(nm, sizeof(nm), "Player %d", i);
        chrono_rift::copy_text(p.name, chrono_rift::kNameSize, nm);
    }
    int ne = 2 + (rand() % 8);
    s->active_enemies = ne;
    for (int i = 0; i < ne; ++i) {
        auto& e     = s->enemies[i];
        e.alive       = 1;
        e.max_hp      = LAST_TWO + 50 + (rand() % 151);
        e.hp          = e.max_hp;
        e.damage      = SECOND_LAST + 10;
        e.speed       = 10 + (rand() % 21);
        e.max_stamina = 150;
        e.stamina     = 0;
        e.stunned     = 0;
        e.actor_id    = i;
        s->enemy_stunned[i] = 0;
        char nm[32]; snprintf(nm, sizeof(nm), "Enemy %d", i);
        chrono_rift::copy_text(e.name, chrono_rift::kNameSize, nm);
    }
    auto& sc = s->artifacts[chrono_rift::kArtifactSolar];
    chrono_rift::copy_text(sc.name, chrono_rift::kNameSize, "Solar Core");
    sc.held_by_kind = -1;
    auto& lb = s->artifacts[chrono_rift::kArtifactLunar];
    chrono_rift::copy_text(lb.name, chrono_rift::kNameSize, "Lunar Blade");
    lb.held_by_kind = -1;
    s->artifacts[chrono_rift::kArtifactEclipse].held_by_kind = -2;
    std::cout << "[arbiter] " << np << " players, " << ne << " enemies\n";
}
// ============================================================
// respawn_wave
// ============================================================
static void respawn_wave(chrono_rift::SharedState* s) {
    int ne = 2 + (rand() % 8);
    s->active_enemies = ne;
    const int LAST_TWO   = 25;
    const int SECOND_LAST= 2;
    for (int i = 0; i < ne; ++i) {
        auto& e = s->enemies[i];
        e.alive       = 1;
        e.max_hp      = LAST_TWO + 50 + (rand() % 151);
        e.hp          = e.max_hp;
        e.damage      = SECOND_LAST + 10;
        e.speed       = 10 + (rand() % 21);
        e.max_stamina = 150;
        e.stamina     = 0;
        e.stunned     = 0;
        e.actor_id    = i;
        s->enemy_stunned[i] = 0;
        char nm[32]; snprintf(nm, sizeof(nm), "Enemy %d", i);
        chrono_rift::copy_text(e.name, chrono_rift::kNameSize, nm);
    }
    char msg[80]; snprintf(msg, sizeof(msg), "New wave: %d enemies", ne);
    log_locked(s, msg);
    std::cout << "[arbiter] " << msg << "\n";
    maybe_spawn_eclipse(s);
}
// ============================================================
// main
// ============================================================
int main() {
    install_signal_mask_and_thread();
    srand(25);
    g_state = chrono_rift::create_shared_state(g_fd);
    g_state->arbiter_pid = getpid();
    std::cout << "[arbiter] shared memory created (pid=" << getpid() << ")\n";
    pid_t hip_pid = -1;
    pid_t asp_pid = -1;
    if (!launch_child_process("./hips", "hips", hip_pid) ||
        !launch_child_process("./asps", "asps", asp_pid)) {
        if (hip_pid > 0) {
            kill(hip_pid, SIGTERM);
        }
        if (asp_pid > 0) {
            kill(asp_pid, SIGTERM);
        }
        chrono_rift::destroy_shared_state(g_state, g_fd);
        return EXIT_FAILURE;
    }
    int np = select_party_size();
    std::cout << "[arbiter] party size selected: " << np << "\n";
    std::cout << "[arbiter] waiting for HIP...\n";
    if (!chrono_rift::sem_wait_interruptible(&g_state->hip_ready, (int&)g_keep_running)) {
        std::cerr << "[arbiter] failed waiting for HIP\n";
        chrono_rift::destroy_shared_state(g_state, g_fd); return EXIT_FAILURE;
    }
    std::cout << "[arbiter] HIP connected\n";
    log_locked(g_state, "HIP connected");
    std::cout << "[arbiter] waiting for ASP...\n";
    if (!chrono_rift::sem_wait_interruptible(&g_state->asp_ready, (int&)g_keep_running)) {
        std::cerr << "[arbiter] failed waiting for ASP\n";
        chrono_rift::destroy_shared_state(g_state, g_fd); return EXIT_FAILURE;
    }
    std::cout << "[arbiter] ASP connected\n";
    log_locked(g_state, "ASP connected");
    sem_post(&g_state->arbiter_ready);
    sem_post(&g_state->arbiter_ready);
    sem_wait(&g_state->state_lock);
    init_entities(g_state, np);
    sem_post(&g_state->state_lock);
    pthread_t render_tid;
    if (pthread_create(&render_tid, nullptr, render_thread_fn, g_state) == 0) {
        pthread_detach(render_tid);
        std::cout << "[arbiter] render thread spawned\n";
    }
    pthread_t dead_tid;
    if (pthread_create(&dead_tid, nullptr, deadlock_monitor_fn, nullptr) == 0) {
        pthread_detach(dead_tid);
        std::cout << "[arbiter] deadlock monitor spawned\n";
    }
    const int TICK_US = 100000;
    while (g_keep_running && g_state->running) {
        usleep(TICK_US);
        sem_wait(&g_state->state_lock);
        for (int i = 0; i < g_state->active_players; ++i) {
            auto& p = g_state->players[i];
            if (!p.alive) {
                continue;
            }
            if (p.stunned) {
                continue;
            }
            p.stamina += p.speed;
            if (p.stamina > p.max_stamina) {
                p.stamina = p.max_stamina;
            }
        }
        for (int i = 0; i < g_state->active_enemies; ++i) {
            auto& e = g_state->enemies[i];
            if (!e.alive) {
                continue;
            }
            if (g_state->enemy_stunned[i]) {
                continue;
            }
            e.stamina += e.speed;
            if (e.stamina > e.max_stamina) {
                e.stamina = e.max_stamina;
            }
        }
        int best_kind = -1, best_idx = -1, best_st = -1;
        for (int i = 0; i < g_state->active_players; ++i) {
            auto& p = g_state->players[i];
            if (!p.alive || p.stunned) {
                continue;
            }
            if (p.stamina >= p.max_stamina && p.stamina > best_st) {
                best_st = p.stamina; best_kind = 0; best_idx = i;
            }
        }
        for (int i = 0; i < g_state->active_enemies; ++i) {
            auto& e = g_state->enemies[i];
            if (!e.alive || g_state->enemy_stunned[i]) {
                continue;
            }
            if (e.stamina >= e.max_stamina && e.stamina > best_st) {
                best_st = e.stamina; best_kind = 1; best_idx = i;
            }
        }
        if (best_kind != -1) {
            g_state->turn_actor_kind  = best_kind;
            g_state->turn_actor_index = best_idx;
            g_state->move_buffer.ready = 0;
            char ready_msg[120];
            snprintf(ready_msg, sizeof(ready_msg), "%s %d ready (stamina=%d)",
                     best_kind==0?"Player":"Enemy", best_idx,
                     best_kind==0 ? g_state->players[best_idx].stamina
                                  : g_state->enemies[best_idx].stamina);
            log_locked(g_state, ready_msg);
            std::cout << "[arbiter] " << ready_msg << "\n";
            int waited = 0;
            while (waited < 30 && g_state->move_buffer.ready == 0 && g_state->running) {
                sem_post(&g_state->state_lock);
                usleep(100000);
                chrono_rift::MoveBuffer gui_mb{};
                bool have_gui = false;
                if (g_renderer) {
                    have_gui = g_renderer->poll_pending_action(gui_mb);
                }
                sem_wait(&g_state->state_lock);
                if (have_gui &&
                    g_state->turn_actor_kind  == best_kind &&
                    g_state->turn_actor_index == best_idx &&
                    g_state->move_buffer.ready == 0) {
                    g_state->move_buffer = gui_mb;
                    g_state->move_buffer.ready = 1;
                }
                ++waited;
            }
            if (g_state->move_buffer.ready == 0) {
                char tmsg[100];
                snprintf(tmsg, sizeof(tmsg), "%s %d timed out, auto-skip",
                         best_kind==0?"Player":"Enemy", best_idx);
                log_locked(g_state, tmsg);
                std::cout << "[arbiter] " << tmsg << "\n";
                if (best_kind == 0) {
                    g_state->players[best_idx].stamina = g_state->players[best_idx].max_stamina / 2;
                } else {
                    g_state->enemies[best_idx].stamina = g_state->enemies[best_idx].max_stamina / 2;
                }
            } else {
                auto& mb = g_state->move_buffer;
                if (mb.actor_kind == 0) {
                    int pi = mb.actor_index;
                    if (pi < 0 || pi >= g_state->active_players) {
                        goto done;
                    }
                    if (mb.action_type == chrono_rift::kActionStrike) {
                        int ti = mb.target_index;
                        if (ti < 0 || ti >= g_state->active_enemies || !g_state->enemies[ti].alive) {
                            ti = -1;
                            for (int t = 0; t < g_state->active_enemies; ++t) {
                                if (g_state->enemies[t].alive) {
                                    ti = t; break;
                                }
                            }
                        }
                        if (ti != -1) {
                            int dmg = g_state->players[pi].damage;
                            g_state->enemies[ti].hp -= dmg;
                            log_fmt(g_state, "Player %d strikes Enemy %d for %d dmg", pi, ti, dmg);
                            if (g_renderer) {
                                g_renderer->notify_action(0, pi, 1, 1, ti);
                            }
                            if (g_state->enemies[ti].hp <= 0) {
                                g_state->enemies[ti].alive = 0;
                                ++g_state->enemies_killed;
                                log_fmt(g_state, "Enemy %d defeated!", ti);
                                maybe_spawn_eclipse(g_state);
                                if (g_state->enemy_weapons[ti] >= 0) {
                                    char dmsg[140];
                                    snprintf(dmsg, sizeof(dmsg),
                                             "Enemy %d had %s; it is lost with the enemy",
                                             ti, g_state->weapons[g_state->enemy_weapons[ti]].name);
                                    log_locked(g_state, dmsg);
                                    std::cout << "[arbiter] " << dmsg << "\n";
                                    g_state->enemy_weapons[ti] = -1;
                                } else if ((rand() % 100) < 55) {
                                    int drop_w = rand() % chrono_rift::kNumWeapons;
                                    g_state->pending_drop_weapon = drop_w;
                                    g_state->pending_drop_enemy  = ti;
                                    char dmsg[120];
                                    snprintf(dmsg, sizeof(dmsg),
                                             "Enemy %d dropped %s! Click Pickup to collect",
                                             ti, g_state->weapons[drop_w].name);
                                    log_locked(g_state, dmsg);
                                    std::cout << "[arbiter] " << dmsg << "\n";
                                }
                            }
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionExhaust) {
                        int ti = mb.target_index;
                        if (ti < 0 || ti >= g_state->active_enemies || !g_state->enemies[ti].alive) {
                            ti = -1;
                            for (int t = 0; t < g_state->active_enemies; ++t) {
                                if (g_state->enemies[t].alive) {
                                    ti = t; break;
                                }
                            }
                        }
                        if (ti != -1) {
                            int dmg = g_state->players[pi].damage;
                            g_state->enemies[ti].stamina -= dmg;
                            if (g_state->enemies[ti].stamina < 0) {
                                g_state->enemies[ti].stamina = 0;
                            }
                            log_fmt(g_state, "Player %d exhausts Enemy %d stamina by %d", pi, ti, dmg);
                            if (g_renderer) {
                                g_renderer->notify_action(0, pi, 8, 1, ti);
                            }
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionHeal) {
                        int heal = g_state->players[pi].max_hp / 10;
                        g_state->players[pi].hp += heal;
                        if (g_state->players[pi].hp > g_state->players[pi].max_hp) {
                            g_state->players[pi].hp = g_state->players[pi].max_hp;
                        }
                        log_fmt(g_state, "Player %d healed for %d HP", pi, heal);
                        if (g_renderer) {
                            g_renderer->notify_action(0, pi, 9, 0, pi);
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionUseWeapon) {
                        int ti = mb.target_index;
                        if (ti < 0 || ti >= g_state->active_enemies || !g_state->enemies[ti].alive) {
                            ti = -1;
                            for (int t = 0; t < g_state->active_enemies; ++t) {
                                if (g_state->enemies[t].alive) {
                                    ti = t; break;
                                }
                            }
                        }
                        int best_dmg = g_state->players[pi].damage;
                        int used_w   = -1;
                        for (int s = 0; s < chrono_rift::kInventorySlots; ++s) {
                            int wi = g_state->inventory_slots[s];
                            if (wi >= 0 && wi < chrono_rift::kNumWeapons) {
                                if (used_w == -1 ||
                                    g_state->weapons[wi].damage > best_dmg) {
                                    best_dmg = g_state->weapons[wi].damage;
                                    used_w   = wi;
                                }
                            }
                        }
                        if (ti != -1) {
                            g_state->enemies[ti].hp -= best_dmg;
                            log_fmt(g_state, "Player %d uses %s on Enemy %d for %d dmg",
                                    pi, used_w>=0?g_state->weapons[used_w].name:"bare hands",
                                    ti, best_dmg);
                            if (g_renderer) {
                                g_renderer->notify_action(0, pi, 10, 1, ti);
                            }
                            if (g_state->enemies[ti].hp <= 0) {
                                g_state->enemies[ti].alive = 0;
                                ++g_state->enemies_killed;
                                log_fmt(g_state, "Enemy %d defeated!", ti);
                                maybe_spawn_eclipse(g_state);
                            }
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionSkip) {
                        g_state->players[pi].stamina = g_state->players[pi].max_stamina / 2;
                        log_fmt(g_state, "Player %d skipped (stamina→50%%)", pi);
                    } else if (mb.action_type == chrono_rift::kActionUltimate) {
                        if (!ultimate_available(g_state)) {
                            log_fmt(g_state, "Player %d: Ultimate blocked (need Solar Core + Lunar Blade in inventory)", pi);
                        } else if (g_state->asp_pid > 0) {
                            kill(g_state->asp_pid, SIGSTOP);
                            alarm(10);
                            log_fmt(g_state, "Player %d ULTIMATE! Enemies frozen for 10s", pi);
                            if (g_renderer) {
                                g_renderer->notify_action(0, pi, 3, 1, -1);
                            }
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionStun) {
                        int ti = mb.target_index;
                        if (ti < 0 || ti >= g_state->active_enemies || !g_state->enemies[ti].alive) {
                            ti = -1;
                            for (int t = 0; t < g_state->active_enemies; ++t) {
                                if (g_state->enemies[t].alive) {
                                    ti = t; break;
                                }
                            }
                        }
                        if (ti != -1) {
                            g_state->enemy_stunned[ti] = 1;
                            if (g_state->asp_pid > 0) {
                                kill(g_state->asp_pid, SIGUSR1);
                            }
                            log_fmt(g_state, "Player %d stunned Enemy %d for 3s", pi, ti);
                            if (g_renderer) {
                                g_renderer->notify_action(0, pi, 4, 1, ti);
                            }
                            struct StunArg { int enemy_idx; };
                            static StunArg stun_args[chrono_rift::kMaxEnemies];
                            stun_args[ti].enemy_idx = ti;
                            pthread_t stun_t;
                            struct StunClear {
                                static void* fn(void* arg) {
                                    StunArg* a = static_cast<StunArg*>(arg);
                                    sleep(3);
                                    if (g_state) {
                                        sem_wait(&g_state->state_lock);
                                        g_state->enemy_stunned[a->enemy_idx] = 0;
                                        char msg[80];
                                        snprintf(msg, sizeof(msg),
                                                 "Enemy %d stun cleared", a->enemy_idx);
                                        log_locked(g_state, msg);
                                        sem_post(&g_state->state_lock);
                                        sem_post(&g_state->enemy_stun_sem[a->enemy_idx]);
                                        std::cout << "[arbiter] Enemy " << a->enemy_idx
                                                  << " stun expired, sem posted\n";
                                    }
                                    return nullptr;
                                }
                            };
                            pthread_create(&stun_t, nullptr, StunClear::fn, &stun_args[ti]);
                            pthread_detach(stun_t);
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionPickup) {
                        int wi = mb.target_index;
                        int pending = g_state->pending_drop_weapon;
                        if (wi < 0) {
                            wi = pending;
                        }
                        if (pending < 0) {
                            log_fmt(g_state, "Player %d: no pickup is available right now", pi);
                        } else if (wi != pending) {
                            log_fmt(g_state, "Player %d: pickup available is %s (%d)",
                                    pi, g_state->weapons[pending].name, pending);
                        } else if (wi >= 0 && wi < chrono_rift::kNumWeapons) {
                            if (pickup_weapon(g_state, pi, wi)) {
                                log_fmt(g_state, "Player %d picked up %s (inventory updated)",
                                        pi, g_state->weapons[wi].name);
                                if (g_state->pending_drop_weapon == wi) {
                                    g_state->pending_drop_weapon = -1;
                                    g_state->pending_drop_enemy  = -1;
                                }
                            } else {
                                log_fmt(g_state, "Player %d: inventory full, could not pick up %s",
                                        pi, g_state->weapons[wi].name);
                            }
                        } else {
                            log_fmt(g_state, "Player %d: invalid pickup request", pi);
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionSwapIn) {
                        int wi = mb.target_index;
                        if (wi >= 0 && wi < chrono_rift::kNumWeapons) {
                            if (swap_in_weapon(g_state, pi, wi)) {
                                log_fmt(g_state, "Player %d swapped in %s from storage (cannot use this turn)",
                                        pi, g_state->weapons[wi].name);
                            } else {
                                log_fmt(g_state, "Player %d: %s not in storage or no space",
                                        pi, g_state->weapons[wi].name);
                            }
                        } else {
                            log_fmt(g_state, "Player %d: invalid weapon index for swap-in", pi);
                        }
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionLock) {
                        handle_artifact_lock(g_state, 0, pi, mb.target_index);
                        g_state->players[pi].stamina = 0;
                    } else if (mb.action_type == chrono_rift::kActionUnlock) {
                        release_all_locks(g_state, 0, pi);
                        log_fmt(g_state, "Player %d released all artifact locks", pi);
                        g_state->players[pi].stamina = 0;
                    }
                } else if (mb.actor_kind == 1) {
                    int ei = mb.actor_index;
                    if (ei < 0 || ei >= g_state->active_enemies) {
                        goto done;
                    }
                    bool skip_flag = false;
                    if (mb.action_type == chrono_rift::kActionStrike) {
                        int ti = -1;
                        for (int t = 0; t < g_state->active_players; ++t) {
                            if (g_state->players[t].alive) {
                                ti = t; break;
                            }
                        }
                        if (ti != -1) {
                            int dmg = g_state->enemies[ei].damage;
                            g_state->players[ti].hp -= dmg;
                            log_fmt(g_state, "Enemy %d attacks Player %d for %d dmg", ei, ti, dmg);
                            if (g_renderer) {
                                g_renderer->notify_action(1, ei, 1, 0, ti);
                            }
                            if (g_state->players[ti].hp <= 0) {
                                g_state->players[ti].alive = 0;
                                log_fmt(g_state, "Player %d defeated!", ti);
                            }
                        }
                    } else if (mb.action_type == chrono_rift::kActionSkip) {
                        skip_flag = true;
                        g_state->enemies[ei].stamina = g_state->enemies[ei].max_stamina / 2;
                        log_fmt(g_state, "Enemy %d skipped (stamina→50%%)", ei);
                    } else if (mb.action_type == chrono_rift::kActionLock) {
                        handle_artifact_lock(g_state, 1, ei, mb.target_index);
                    } else if (mb.action_type == chrono_rift::kActionUnlock) {
                        release_all_locks(g_state, 1, ei);
                        log_fmt(g_state, "Enemy %d released artifact locks", ei);
                    }
                    if (!skip_flag) {
                        g_state->enemies[ei].stamina = 0;
                    }
                }
            }
            done:
            g_state->turn_actor_kind  = -1;
            g_state->turn_actor_index = -1;
        }
        if (g_state->enemies_killed >= 10) {
            g_state->winner = 0;
            log_locked(g_state, "=== PLAYERS WIN ===");
            std::cout << "[arbiter] === PLAYERS WIN ===\n";
            sem_post(&g_state->state_lock);
            break;
        }
        int alive_p = 0, alive_e = 0;
        for (int i = 0; i < g_state->active_players; ++i) {
            if (g_state->players[i].alive) {
                ++alive_p;
            }
        }
        for (int i = 0; i < g_state->active_enemies; ++i) {
            if (g_state->enemies[i].alive) {
                ++alive_e;
            }
        }
        if (alive_p == 0) {
            g_state->winner = 1;
            log_locked(g_state, "=== ENEMIES WIN ===");
            std::cout << "[arbiter] === ENEMIES WIN ===\n";
            sem_post(&g_state->state_lock);
            break;
        }
        if (alive_e == 0 && g_state->enemies_killed < 10) {
            respawn_wave(g_state);
        }
        sem_post(&g_state->state_lock);
    }
    g_state->running = 0;
    for (int i = 0; i < chrono_rift::kMaxEnemies; ++i) {
        sem_post(&g_state->enemy_stun_sem[i]);
    }
    sem_post(&g_state->arbiter_ready);
    sem_post(&g_state->arbiter_ready);
    sleep(2);
    chrono_rift::destroy_shared_state(g_state, g_fd);
    std::cout << "[arbiter] shutdown complete\n";
    return EXIT_SUCCESS;
}