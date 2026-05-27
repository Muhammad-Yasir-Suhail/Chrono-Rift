#pragma once
#include "ipc.h"
#include <SFML/Graphics.hpp>
#include <map>
#include <string>
#include <iostream>
#include <vector>
#include <pthread.h>
#include <cmath>
#include <cstring>
namespace chrono_rift {
// ============================================================
// RendererActionQueue
// ============================================================
struct RendererActionQueue {
    static constexpr int kCapacity = 16;
    chrono_rift::MoveBuffer items[kCapacity];
    int  head  = 0;
    int  tail  = 0;
    int  count = 0;
    pthread_mutex_t mtx;
    void init() {
        pthread_mutex_init(&mtx, nullptr);
    }
    void destroy() {
        pthread_mutex_destroy(&mtx);
    }
    void push(const chrono_rift::MoveBuffer& mb) {
        pthread_mutex_lock(&mtx);
        if (count < kCapacity) {
            items[tail] = mb;
            tail = (tail + 1) % kCapacity;
            ++count;
        }
        pthread_mutex_unlock(&mtx);
    }
    bool pop(chrono_rift::MoveBuffer& out) {
        pthread_mutex_lock(&mtx);
        if (count == 0) {
            pthread_mutex_unlock(&mtx); return false;
        }
        out  = items[head];
        head = (head + 1) % kCapacity;
        --count;
        pthread_mutex_unlock(&mtx);
        return true;
    }
};
// ============================================================
// AnimKind
// ============================================================
enum class AnimKind {
    PROJECTILE,
    WAVE,
    SLASH,
    HEAL,
    STUN,
    ULTIMATE,
};
// ============================================================
// Animation
// ============================================================
struct Animation {
    AnimKind      kind;
    sf::Vector2f  src;
    sf::Vector2f  dst;
    sf::Color     color;
    float         duration;
    float         elapsed;
    bool          done = false;
};
// ============================================================
// GameRenderer
// ============================================================
class GameRenderer {
private:
    sf::RenderWindow      window;
    SharedState*          state;
    std::map<std::string, sf::Texture> textures;
    sf::Font              font;
    bool                  font_loaded = false;
    bool                  running;
    int                   selected_target;
    RendererActionQueue   action_queue;
    sf::Clock             frame_clock;
    std::vector<Animation> animations;
    float player_flash[kMaxPlayers] = {};
    float enemy_flash[kMaxEnemies]  = {};
    int       last_clicked_button = -1;
    sf::Clock button_click_timer;
    static constexpr float kAnimDuration = 0.6f;
    static constexpr int kW  = 1280;
    static constexpr int kH  = 720;
    static constexpr int kSS = 64;
    int cached_turn_kind  = -1;
    int cached_turn_index = -1;
    int cached_kills      = 0;
    int cached_drop_weapon = -1;
    int cached_winner      = -1;
public:
    explicit GameRenderer(SharedState* s)
        : window(sf::VideoMode(kW, kH), "Chrono Rift"),
          state(s), running(true), selected_target(-1)
    {
        window.setFramerateLimit(60);
        window.setVerticalSyncEnabled(true);
        action_queue.init();
        load_assets();
    }
    ~GameRenderer() {
        action_queue.destroy();
    }
    bool is_running() const {
        return running && window.isOpen();
    }
    bool poll_pending_action(chrono_rift::MoveBuffer& out) {
        return action_queue.pop(out);
    }
    // ============================================================
    // request_shutdown
    // ============================================================
    void request_shutdown() {
        running = false;
        if (state) {
            sem_wait(&state->state_lock);
            state->running = 0;
            sem_post(&state->state_lock);
        }
        window.close();
    }
    // ============================================================
    // notify_action
    // ============================================================
    void notify_action(int actor_kind, int actor_index,
                       int action_type, int target_kind, int target_index) {
        sf::Vector2f src = entity_centre(actor_kind, actor_index);
        sf::Vector2f dst = entity_centre(target_kind, target_index);
        Animation a{};
        a.src     = src;
        a.dst     = dst;
        a.elapsed = 0.f;
        a.done    = false;
        switch (action_type) {
            case 1:
                a.kind     = AnimKind::PROJECTILE;
                a.color    = sf::Color(255, 140, 30);
                a.duration = 0.45f;
                animations.push_back(a);
                spawn_slash(dst, sf::Color(255, 200, 80));
                set_flash(target_kind, target_index);
                break;
            case 8:
                a.kind     = AnimKind::PROJECTILE;
                a.color    = sf::Color(60, 140, 255);
                a.duration = 0.45f;
                animations.push_back(a);
                spawn_wave(dst, sf::Color(80, 160, 255));
                set_flash(target_kind, target_index);
                break;
            case 9:
                a.kind     = AnimKind::HEAL;
                a.color    = sf::Color(60, 220, 80);
                a.duration = 0.8f;
                a.src      = src;
                a.dst      = src;
                animations.push_back(a);
                break;
            case 10:
                a.kind     = AnimKind::PROJECTILE;
                a.color    = sf::Color(220, 60, 60);
                a.duration = 0.4f;
                animations.push_back(a);
                spawn_slash(dst, sf::Color(255, 80, 80));
                set_flash(target_kind, target_index);
                break;
            case 3:
                a.kind     = AnimKind::ULTIMATE;
                a.color    = sf::Color(255, 220, 60, 180);
                a.duration = 1.0f;
                a.src      = {kW / 2.f, kH / 2.f};
                a.dst      = {kW / 2.f, kH / 2.f};
                animations.push_back(a);
                break;
            case 4:
                a.kind     = AnimKind::STUN;
                a.color    = sf::Color(255, 230, 0);
                a.duration = 1.2f;
                a.dst      = dst;
                animations.push_back(a);
                break;
            default:
                break;
        }
    }
    // ============================================================
    // load_assets
    // ============================================================
    void load_assets() {
        font_loaded = font.loadFromFile("assets/fonts/PressStart2P-Regular.ttf");
        if (!font_loaded) {
            std::cout << "[renderer] font not found" << std::endl;
        }
        auto load_tex = [&](const std::string& key, const std::string& path) {
            sf::Texture t;
            if (t.loadFromFile(path)) {
                textures[key] = t;
            }
        };
        for (int i = 1; i <= 4; ++i) {
            load_tex("player" + std::to_string(i),
                     "assets/sprites/player" + std::to_string(i) + ".png");
        }
        for (int i = 1; i <= 9; ++i) {
            load_tex("enemy" + std::to_string(i),
                     "assets/sprites/enemy" + std::to_string(i) + ".png");
        }
        load_tex("artifact_solar",   "assets/artifacts/solar_core_artifact.png");
        load_tex("artifact_lunar",   "assets/artifacts/lunar_blade_artifact.png");
        load_tex("artifact_eclipse", "assets/artifacts/eclipse_relic.png");
        load_tex("background",       "assets/background/battle_bg.png");
    }
    // ============================================================
    // update_and_render
    // ============================================================
    void update_and_render() {
        float dt = frame_clock.restart().asSeconds();
        sf::Event ev;
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                request_shutdown();
                return;
            }
            if (ev.type == sf::Event::KeyPressed &&
                ev.key.code == sf::Keyboard::Escape) {
                request_shutdown();
                return;
            } else if (ev.type == sf::Event::MouseButtonPressed &&
                       ev.mouseButton.button == sf::Mouse::Left) {
                handle_click(sf::Vector2i(ev.mouseButton.x,
                                          ev.mouseButton.y));
            }
        }
        if (!window.isOpen()) {
            request_shutdown();
            return;
        }
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (player_flash[i] > 0.f) {
                player_flash[i] -= dt;
            }
        }
        for (int i = 0; i < kMaxEnemies; ++i) {
            if (enemy_flash[i] > 0.f) {
                enemy_flash[i] -= dt;
            }
        }
        for (auto& a : animations) {
            a.elapsed += dt;
        }
        window.clear(sf::Color(18, 18, 28));
        if (textures.count("background")) {
            sf::Sprite bg(textures["background"]);
            window.draw(bg);
        }
        sem_wait(&state->state_lock);
        draw_players();
        draw_enemies();
        draw_inventory_panel();
        draw_action_log();
        cached_turn_kind  = state->turn_actor_kind;
        cached_turn_index = state->turn_actor_index;
        cached_kills      = state->enemies_killed;
        cached_drop_weapon = state->pending_drop_weapon;
        cached_winner     = state->winner;
        sem_post(&state->state_lock);
        draw_animations(dt);
        draw_controls(cached_turn_kind, cached_turn_index, cached_kills);
        draw_end_screen(cached_winner);
        window.display();
        std::vector<Animation> alive;
        for (auto& a : animations) {
            if (a.elapsed < a.duration) {
                alive.push_back(a);
            }
        }
        animations = alive;
    }
private:
    // ============================================================
    // player_pos
    // ============================================================
    sf::Vector2f player_pos(int i) {
        return { 10.f + kSS / 2.f, 50.f + i * 130.f + kSS / 2.f };
    }
    // ============================================================
    // enemy_pos
    // ============================================================
    sf::Vector2f enemy_pos(int i) {
        return { float(kW - 10 - kSS / 2), 50.f + i * 90.f + kSS / 2.f };
    }
    // ============================================================
    // entity_centre
    // ============================================================
    sf::Vector2f entity_centre(int kind, int index) {
        if (kind == 0) {
            return player_pos(index);
        }
        return enemy_pos(index);
    }
    // ============================================================
    // set_flash
    // ============================================================
    void set_flash(int kind, int index) {
        if (kind == 0 && index < kMaxPlayers) {
            player_flash[index] = 0.35f;
        }
        if (kind == 1 && index < kMaxEnemies) {
            enemy_flash[index]  = 0.35f;
        }
    }
    // ============================================================
    // spawn_slash
    // ============================================================
    void spawn_slash(sf::Vector2f pos, sf::Color col) {
        Animation s{};
        s.kind     = AnimKind::SLASH;
        s.color    = col;
        s.duration = 0.35f;
        s.elapsed  = 0.f;
        s.dst      = pos;
        s.src      = pos;
        animations.push_back(s);
    }
    // ============================================================
    // spawn_wave
    // ============================================================
    void spawn_wave(sf::Vector2f pos, sf::Color col) {
        Animation w{};
        w.kind     = AnimKind::WAVE;
        w.color    = col;
        w.duration = 0.5f;
        w.elapsed  = 0.f;
        w.dst      = pos;
        w.src      = pos;
        animations.push_back(w);
    }
    // ============================================================
    // draw_animations
    // ============================================================
    void draw_animations(float /*dt*/) {
        for (auto& a : animations) {
            if (a.elapsed >= a.duration) {
                continue;
            }
            float t = a.elapsed / a.duration;
            switch (a.kind) {
                case AnimKind::PROJECTILE: {
                    sf::Vector2f pos = a.src + (a.dst - a.src) * t;
                    float radius = 10.f + 4.f * std::sin(t * 3.14159f);
                    sf::Uint8 alpha = static_cast<sf::Uint8>(255 * (1.f - t * 0.4f));
                    sf::CircleShape glow(radius * 2.2f);
                    glow.setOrigin(radius * 2.2f, radius * 2.2f);
                    glow.setPosition(pos);
                    glow.setFillColor(sf::Color(a.color.r, a.color.g, a.color.b,
                                                static_cast<sf::Uint8>(alpha / 3)));
                    window.draw(glow);
                    sf::CircleShape orb(radius);
                    orb.setOrigin(radius, radius);
                    orb.setPosition(pos);
                    orb.setFillColor(sf::Color(a.color.r, a.color.g, a.color.b, alpha));
                    window.draw(orb);
                    for (int tr = 1; tr <= 4; ++tr) {
                        float tt = t - tr * 0.06f;
                        if (tt < 0.f) {
                            continue;
                        }
                        sf::Vector2f tp = a.src + (a.dst - a.src) * tt;
                        float tr_r = radius * (1.f - tr * 0.18f);
                        sf::Uint8 ta = static_cast<sf::Uint8>(
                            alpha * (1.f - tr * 0.22f));
                        sf::CircleShape trail(tr_r);
                        trail.setOrigin(tr_r, tr_r);
                        trail.setPosition(tp);
                        trail.setFillColor(sf::Color(a.color.r, a.color.g,
                                                     a.color.b, ta));
                        window.draw(trail);
                    }
                    break;
                }
                case AnimKind::SLASH: {
                    sf::Uint8 alpha = static_cast<sf::Uint8>(255 * (1.f - t));
                    sf::Color c(a.color.r, a.color.g, a.color.b, alpha);
                    float len = 40.f + 20.f * t;
                    auto draw_line = [&](float angle_deg) {
                        float angle = angle_deg * 3.14159f / 180.f;
                        sf::Vector2f dir(std::cos(angle), std::sin(angle));
                        sf::Vertex line[2] = {
                            sf::Vertex(a.dst - dir * len, c),
                            sf::Vertex(a.dst + dir * len, c)
                        };
                        window.draw(line, 2, sf::Lines);
                    };
                    draw_line(-45.f);
                    draw_line(0.f);
                    draw_line(45.f);
                    break;
                }
                case AnimKind::WAVE: {
                    float radius = 20.f + 60.f * t;
                    sf::Uint8 alpha = static_cast<sf::Uint8>(255 * (1.f - t));
                    sf::CircleShape ring(radius, 32);
                    ring.setOrigin(radius, radius);
                    ring.setPosition(a.dst);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(sf::Color(a.color.r, a.color.g,
                                                   a.color.b, alpha));
                    ring.setOutlineThickness(4.f * (1.f - t) + 1.f);
                    window.draw(ring);
                    break;
                }
                case AnimKind::HEAL: {
                    for (int p = 0; p < 6; ++p) {
                        float offset  = float(p) / 6.f;
                        float pt      = std::fmod(t + offset, 1.f);
                        float px      = a.src.x + (p % 3 - 1) * 18.f
                                        + 8.f * std::sin(pt * 6.28f);
                        float py      = a.src.y - pt * 70.f;
                        float radius  = 6.f * (1.f - pt);
                        sf::Uint8 alpha = static_cast<sf::Uint8>(200 * (1.f - pt));
                        sf::CircleShape particle(radius);
                        particle.setOrigin(radius, radius);
                        particle.setPosition(px, py);
                        particle.setFillColor(sf::Color(60, 220, 80, alpha));
                        window.draw(particle);
                    }
                    break;
                }
                case AnimKind::STUN: {
                    for (int s = 0; s < 5; ++s) {
                        float angle = t * 6.28f * 2.f
                                      + s * (6.28f / 5.f);
                        float orbit_r = 36.f;
                        float px = a.dst.x + orbit_r * std::cos(angle);
                        float py = a.dst.y - 30.f + orbit_r * 0.5f
                                   * std::sin(angle);
                        float radius = 7.f;
                        sf::Uint8 alpha = static_cast<sf::Uint8>(
                            230 * (1.f - t * 0.4f));
                        sf::CircleShape star(radius, 5);
                        star.setOrigin(radius, radius);
                        star.setPosition(px, py);
                        star.setFillColor(sf::Color(255, 230, 0, alpha));
                        window.draw(star);
                    }
                    break;
                }
                case AnimKind::ULTIMATE: {
                    sf::Uint8 alpha = static_cast<sf::Uint8>(
                        180 * std::sin(t * 3.14159f));
                    sf::RectangleShape flash(sf::Vector2f(kW, kH));
                    flash.setFillColor(sf::Color(255, 220, 60, alpha));
                    window.draw(flash);
                    float ring_r = 50.f + 500.f * t;
                    sf::CircleShape ring(ring_r, 64);
                    ring.setOrigin(ring_r, ring_r);
                    ring.setPosition(kW / 2.f, kH / 2.f);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(sf::Color(255, 240, 120,
                                                   static_cast<sf::Uint8>(
                                                       200 * (1.f - t))));
                    ring.setOutlineThickness(6.f);
                    window.draw(ring);
                    break;
                }
            }
        }
    }
    // ============================================================
    // queue_action
    // ============================================================
    void queue_action(int player_idx, int action_type, int target_idx) {
        chrono_rift::MoveBuffer mb{};
        mb.ready        = 1;
        mb.actor_kind   = 0;
        mb.actor_index  = player_idx;
        mb.action_type  = action_type;
        mb.target_index = target_idx;
        mb.damage       = state->players[player_idx].damage;
        action_queue.push(mb);
    }
    // ============================================================
    // handle_click
    // ============================================================
    void handle_click(const sf::Vector2i& pos) {
        sem_wait(&state->state_lock);
        int ak   = state->turn_actor_kind;
        int ai   = state->turn_actor_index;
        int mrdy = state->move_buffer.ready;
        int ne   = state->active_enemies;
        sem_post(&state->state_lock);
        if (pos.x >= kW - 220) {
            for (int i = 0; i < ne && i < kMaxEnemies; ++i) {
                float ey = 50.f + i * 90.f;
                if (pos.y >= ey && pos.y <= ey + kSS) {
                    selected_target = i;
                    return;
                }
            }
        }
        if (ak != 0 || mrdy != 0) {
            return;
        }
        const int base_y  = kH - 72;
        const int bh      = 32;
        const int bw      = 88;
        const int gap     = 6;
        const int start_x = 10;
        if (pos.y < base_y || pos.y > base_y + bh) {
            return;
        }
        struct Btn { int action; };
        static const Btn btns[10] = {
            {1},{8},{9},{10},{2},{3},{4},{5},{6},{7}
        };
        for (int b = 0; b < 10; ++b) {
            int bx = start_x + b * (bw + gap);
            if (pos.x >= bx && pos.x <= bx + bw) {
                last_clicked_button = btns[b].action;
                button_click_timer.restart();
                bool needs_tgt = (btns[b].action == 1 ||
                                  btns[b].action == 4 ||
                                  btns[b].action == 8 ||
                                  btns[b].action == 10);
                queue_action(ai, btns[b].action,
                             needs_tgt ? selected_target : -1);
                return;
            }
        }
    }
    // ============================================================
    // draw_sprite
    // ============================================================
    void draw_sprite(int x, int y, const std::string& key,
                     const sf::Color& fallback,
                     float flash_t = 0.f) {
        auto it = textures.find(key);
        if (it != textures.end()) {
            sf::Sprite s(it->second);
            auto sz = it->second.getSize();
            if (sz.x > 0 && sz.y > 0) {
                s.setScale(float(kSS)/sz.x, float(kSS)/sz.y);
            }
            s.setPosition(float(x), float(y));
            if (flash_t > 0.f) {
                sf::Uint8 r = static_cast<sf::Uint8>(
                    255 * std::min(1.f, flash_t * 6.f));
                s.setColor(sf::Color(255, 255 - r, 255 - r, 255));
            }
            window.draw(s);
        } else {
            sf::Color c = fallback;
            if (flash_t > 0.f) {
                c = sf::Color(255, 80, 80);
            }
            sf::RectangleShape r(sf::Vector2f(kSS, kSS));
            r.setPosition(float(x), float(y));
            r.setFillColor(c);
            window.draw(r);
        }
    }
    // ============================================================
    // draw_text
    // ============================================================
    void draw_text(const std::string& str, float x, float y,
                   unsigned sz, const sf::Color& col) {
        if (!font_loaded) {
            return;
        }
        sf::Text t(str, font, sz);
        t.setPosition(x, y);
        t.setFillColor(col);
        window.draw(t);
    }
    // ============================================================
    // draw_bar
    // ============================================================
    void draw_bar(int x, int y, int w, int h,
                  int val, int max_val,
                  const sf::Color& fill, const char* lbl) {
        sf::RectangleShape bg(sf::Vector2f(w, h));
        bg.setPosition(x, y);
        bg.setFillColor(sf::Color(45, 45, 45));
        window.draw(bg);
        if (max_val > 0) {
            int filled = std::max(0, std::min(w, val * w / max_val));
            sf::RectangleShape bar(sf::Vector2f(filled, h));
            bar.setPosition(x, y);
            bar.setFillColor(fill);
            window.draw(bar);
        }
        draw_text(lbl, x + 2, y, 8, sf::Color::White);
    }
    // ============================================================
    // draw_players
    // ============================================================
    void draw_players() {
        for (int i = 0; i < state->active_players && i < kMaxPlayers; ++i) {
            EntityState& p = state->players[i];
            int x = 10, y = 50 + i * 130;
            if (state->turn_actor_kind == 0 &&
                state->turn_actor_index == i && p.alive) {
                sf::RectangleShape hl(sf::Vector2f(kSS + 6, kSS + 6));
                hl.setPosition(x - 3, y - 3);
                hl.setFillColor(sf::Color::Transparent);
                hl.setOutlineColor(sf::Color::Cyan);
                hl.setOutlineThickness(2.f);
                window.draw(hl);
            }
            draw_sprite(x, y, "player" + std::to_string(i + 1),
                        p.alive ? sf::Color(90,50,140) : sf::Color(60,60,60),
                        player_flash[i]);
            draw_text(p.name, x + kSS + 6, y, 11,
                      p.alive ? sf::Color::White : sf::Color(120,120,120));
            if (!p.alive) {
                draw_text("DEAD", x + kSS + 6, y + 18, 11, sf::Color::Red);
            } else {
                draw_bar(x+kSS+6, y+18, 140, 10,
                         p.hp, p.max_hp, sf::Color::Green, "HP");
                draw_bar(x+kSS+6, y+33, 140, 10,
                         p.stamina, p.max_stamina,
                         sf::Color(50,100,220), "ST");
            }
        }
    }
    // ============================================================
    // draw_enemies
    // ============================================================
    void draw_enemies() {
        for (int i = 0; i < state->active_enemies && i < kMaxEnemies; ++i) {
            EntityState& e = state->enemies[i];
            if (!e.alive) {
                continue;
            }
            int x = kW - 10 - kSS;
            int y = 50 + i * 90;
            if (i == selected_target) {
                sf::RectangleShape hl(sf::Vector2f(kSS + 6, kSS + 6));
                hl.setPosition(x - 3, y - 3);
                hl.setFillColor(sf::Color(255,220,0,50));
                hl.setOutlineColor(sf::Color::Yellow);
                hl.setOutlineThickness(2.f);
                window.draw(hl);
            }
            if (state->turn_actor_kind == 1 &&
                state->turn_actor_index == i) {
                sf::RectangleShape hl(sf::Vector2f(kSS + 6, kSS + 6));
                hl.setPosition(x - 3, y - 3);
                hl.setFillColor(sf::Color::Transparent);
                hl.setOutlineColor(sf::Color(255,120,0));
                hl.setOutlineThickness(2.f);
                window.draw(hl);
            }
            draw_sprite(x, y, "enemy" + std::to_string(i + 1),
                        sf::Color(180,60,60), enemy_flash[i]);
            draw_text(e.name, x - 160, y, 11, sf::Color::White);
            draw_bar(x-160, y+18, 140, 10,
                     e.hp, e.max_hp, sf::Color::Green, "HP");
            draw_bar(x-160, y+33, 140, 10,
                     e.stamina, e.max_stamina,
                     sf::Color(50,100,220), "ST");
        }
    }
    // ============================================================
    // draw_inventory_panel
    // ============================================================
    void draw_inventory_panel() {
        const int px=10, py=kH-220, pw=400, ph=140;
        sf::RectangleShape panel(sf::Vector2f(pw, ph));
        panel.setPosition(px, py);
        panel.setFillColor(sf::Color(18,18,42,215));
        panel.setOutlineColor(sf::Color(60,60,90));
        panel.setOutlineThickness(1.f);
        window.draw(panel);
        draw_text("Inventory (20 slots)", px+8, py+5, 11, sf::Color::Yellow);
        const int sw=28, sh=20, sgap=3;
        const int gx=px+8, gy=py+22;
        auto slot_color = [](int a) -> sf::Color {
            switch (a) {
                case 0: return sf::Color(70,190,70);
                case 1: return sf::Color(70,130,210);
                case 2: return sf::Color(200,150,50);
                case 3: return sf::Color(175,85,180);
                case 4: return sf::Color(210,140,60);
                case 5: return sf::Color(190,80,80);
                case 6: return sf::Color(80,175,185);
                case 7: return sf::Color(175,175,110);
                default: return sf::Color(42,42,62);
            }
        };
        auto weapon_name = [](int idx) -> const char* {
            static const char* names[] = {
                "Solar Core", "Lunar Blade", "Iron Halberd", "Venom Dagger",
                "Thunderstaff", "Obsidian Axe", "Frostbow", "Splinter Stick"
            };
            if (idx < 0 || idx >= kNumWeapons) {
                return "Unknown";
            }
            return names[idx];
        };
        auto weapon_short = [](int idx) -> const char* {
            static const char* names[] = {"So", "Lu", "Ha", "Ve", "Th", "Ob", "Fr", "Sp"};
            if (idx < 0 || idx >= kNumWeapons) {
                return "??";
            }
            return names[idx];
        };
        int occupied_cells = 0;
        bool seen[kNumWeapons] = {};
        std::string held = "Held: ";
        bool any_held = false;
        for (int s = 0; s < kInventorySlots; ++s) {
            int col=s%10, row=s/10;
            int sx=gx+col*(sw+sgap), sy=gy+row*(sh+sgap);
            int art=state->inventory_slots[s];
            int owner = art;
            if (art == kWeaponCont && s > 0) {
                owner = state->inventory_slots[s-1];
            }
            bool occupied = (art >= 0 || art == kWeaponCont);
            if (occupied) {
                ++occupied_cells;
                if (art >= 0 && !seen[art]) {
                    if (any_held) {
                        held += ", ";
                    }
                    held += weapon_name(art);
                    seen[art] = true;
                    any_held = true;
                }
            }
            sf::RectangleShape cell(sf::Vector2f(sw, sh));
            cell.setPosition(sx, sy);
            sf::Color fill = slot_color(owner);
            if (art == kWeaponCont) {
                fill = sf::Color(std::max(0, fill.r - 35), std::max(0, fill.g - 35), std::max(0, fill.b - 35));
            }
            cell.setFillColor(occupied ? fill : sf::Color(42,42,62));
            cell.setOutlineColor(occupied ? sf::Color(240,240,240) : sf::Color(12,12,22));
            cell.setOutlineThickness(1.f);
            window.draw(cell);
            if (art >= 0) {
                bool first = (s==0)||(state->inventory_slots[s-1]!=art);
                if (first) {
                    draw_text(weapon_short(art), sx+3, sy+1, 8, sf::Color::Black);
                    char slot_buf[8];
                    snprintf(slot_buf, sizeof(slot_buf), "%d", state->weapons[art].slots);
                    draw_text(slot_buf, sx+16, sy+9, 7, sf::Color::Black);
                }
            } else if (art == kWeaponCont && owner >= 0 && owner < kNumWeapons) {
                draw_text(".", sx+10, sy+6, 8, sf::Color(20,20,20));
            }
        }
        int line_y = gy + 2*(sh+sgap) + 6;
        if (!any_held) {
            held += "(empty)";
        }
        draw_text(held, px+8, line_y, 9,
                  any_held ? sf::Color::Cyan : sf::Color(110,110,110));
        char occ_buf[64];
        snprintf(occ_buf, sizeof(occ_buf), "Occupied cells: %d / %d",
                 occupied_cells, kInventorySlots);
        draw_text(occ_buf, px+8, line_y+15, 9, sf::Color(200,200,200));
    }
    // ============================================================
    // draw_action_log
    // ============================================================
    void draw_action_log() {
        const int lx=kW-315, ly=kH-165, lw=305, lh=155;
        sf::RectangleShape panel(sf::Vector2f(lw, lh));
        panel.setPosition(lx, ly);
        panel.setFillColor(sf::Color(18,18,42,215));
        panel.setOutlineColor(sf::Color(60,60,90));
        panel.setOutlineThickness(1.f);
        window.draw(panel);
        draw_text("Action Log", lx+8, ly+4, 11, sf::Color::Yellow);
        int n = std::min(state->action_log_count, 5);
        int start = (state->action_log_head - n + kActionLogEntries)
                    % kActionLogEntries;
        for (int i = 0; i < n; ++i) {
            int idx = (start + i) % kActionLogEntries;
            draw_text(state->action_log[idx],
                      lx+5, ly+22+i*26, 8, sf::Color(200,200,200));
        }
    }
    // ============================================================
    // anim_t_btn
    // ============================================================
    float anim_t_btn() {
        float e = button_click_timer.getElapsedTime().asSeconds();
        if (e > kAnimDuration) {
            last_clicked_button = -1; return 0.f;
        }
        return 1.f - e / kAnimDuration;
    }
    // ============================================================
    // draw_button
    // ============================================================
    void draw_button(int x, int y, int w, int h,
                     const char* lbl, const sf::Color& base, int btn_id) {
        float ap = anim_t_btn();
        sf::Color c = base;
        if (last_clicked_button == btn_id && ap > 0.f) {
            c = sf::Color(int(base.r+(255-base.r)*ap),
                          int(base.g+(255-base.g)*ap),
                          int(base.b+(255-base.b)*ap));
        }
        sf::RectangleShape btn(sf::Vector2f(w, h));
        btn.setPosition(x, y);
        btn.setFillColor(c);
        btn.setOutlineColor(sf::Color(10,10,10));
        btn.setOutlineThickness(1.5f);
        window.draw(btn);
        draw_text(lbl, x+4, y+6, 9, sf::Color::White);
    }
    // ============================================================
    // draw_controls
    // ============================================================
    void draw_controls(int turn_kind, int turn_index, int enemies_killed) {
        auto weapon_name = [](int idx) -> const char* {
            static const char* names[] = {
                "Solar Core", "Lunar Blade", "Iron Halberd", "Venom Dagger",
                "Thunderstaff", "Obsidian Axe", "Frostbow", "Splinter Stick"
            };
            if (idx < 0 || idx >= kNumWeapons) {
                return "none";
            }
            return names[idx];
        };

        sf::RectangleShape top_strip(sf::Vector2f(kW-20, 30));
        top_strip.setPosition(10, 8);
        top_strip.setFillColor(sf::Color(12,12,22,220));
        top_strip.setOutlineColor(sf::Color(55,55,80));
        top_strip.setOutlineThickness(1.f);
        window.draw(top_strip);

        char top_buf[220];
        if (cached_drop_weapon >= 0 && cached_drop_weapon < kNumWeapons) {
            int pickup_slots = state->weapons[cached_drop_weapon].slots;
            snprintf(top_buf, sizeof(top_buf),
                     "Pickup available: %s (%d slots, action 5 %d)   |   Enemies killed: %d / 10",
                     weapon_name(cached_drop_weapon), pickup_slots,
                     cached_drop_weapon, enemies_killed);
            draw_text(top_buf, 18, 16, 10, sf::Color(255,210,60));
        } else {
            snprintf(top_buf, sizeof(top_buf),
                     "Pickup available: none   |   Enemies killed: %d / 10",
                     enemies_killed);
            draw_text(top_buf, 18, 16, 10, sf::Color(160,255,160));
        }

        const int base_y=kH-72, bh=32, bw=88, gap=6, start_x=10;
        sf::RectangleShape strip(sf::Vector2f(kW-20, 50));
        strip.setPosition(start_x-4, base_y-9);
        strip.setFillColor(sf::Color(12,12,22,220));
        strip.setOutlineColor(sf::Color(55,55,80));
        strip.setOutlineThickness(1.f);
        window.draw(strip);
        struct Btn { const char* label; sf::Color color; int id; };
        static const Btn btns[10] = {
            {"Strike",   sf::Color(85, 55,115), 1},
            {"Exhaust",  sf::Color(45, 80,115), 8},
            {"Heal",     sf::Color(35,100, 65), 9},
            {"Weapon",   sf::Color(80, 80, 35),10},
            {"Skip",     sf::Color(65, 65, 65), 2},
            {"Ultimate", sf::Color(135,65, 25), 3},
            {"Stun",     sf::Color(115,85, 25), 4},
            {"Pickup",   sf::Color(35, 95, 85), 5},
            {"Lock",     sf::Color(115,65, 65), 6},
            {"Unlock",   sf::Color(85, 85, 35), 7},
        };
        for (int b = 0; b < 10; ++b) {
            draw_button(start_x+b*(bw+gap), base_y,
                        bw, bh, btns[b].label, btns[b].color, btns[b].id);
        }
        std::string turn_str;
        if (turn_kind==0) {
            turn_str = ">>> Player "+std::to_string(turn_index)+" Turn <<<  ";
            turn_str += selected_target>=0
                ? "[Target: Enemy "+std::to_string(selected_target)+"]"
                : "[Click enemy to select target]";
        } else if (turn_kind==1) {
            turn_str = ">>> Enemy "+std::to_string(turn_index)+" Turn (AI) <<<";
        } else {
            turn_str = "Waiting...";
        }
        draw_text(turn_str, start_x, base_y-22, 11, sf::Color::Cyan);
    }
    // ============================================================
    // draw_end_screen
    // ============================================================
    void draw_end_screen(int winner) {
        if (winner != 0 && winner != 1) {
            return;
        }
        sf::RectangleShape overlay{sf::Vector2f(float(kW), float(kH))};
        overlay.setPosition(0.f, 0.f);
        overlay.setFillColor(sf::Color(0, 0, 0, 165));
        window.draw(overlay);

        sf::RectangleShape panel{sf::Vector2f(520.f, 210.f)};
        panel.setPosition((kW - 520.f) / 2.f, (kH - 210.f) / 2.f);
        panel.setFillColor(sf::Color(20, 20, 34, 240));
        panel.setOutlineColor(winner == 0 ? sf::Color(90, 220, 110) : sf::Color(230, 90, 90));
        panel.setOutlineThickness(3.f);
        window.draw(panel);

        const char* title = winner == 0 ? "PLAYERS WIN" : "ENEMIES WIN";
        const char* subtitle = winner == 0
            ? "The team reached the kill target."
            : "All players were defeated.";
        draw_text(title, (kW - 280.f) / 2.f, (kH - 210.f) / 2.f + 48.f, 20,
                  winner == 0 ? sf::Color(120, 255, 140) : sf::Color(255, 120, 120));
        draw_text(subtitle, (kW - 280.f) / 2.f, (kH - 210.f) / 2.f + 96.f, 11,
                  sf::Color::White);
        draw_text("Close the window to exit.", (kW - 280.f) / 2.f,
                  (kH - 210.f) / 2.f + 134.f, 10, sf::Color(200, 200, 200));
    }
};
} // namespace chrono_rift