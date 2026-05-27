#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
// ============================================================
// Roll Number: 25
//   Last digit         = 5   -> Player Damage = 5 + 10 = 15
//   Second-last digit  = 2   -> Enemy  Damage = 2 + 10 = 12
//   Last 2 digits      = 25  -> Enemy  HP base = 25
//   Full number as seed: srand(25)
// ============================================================
namespace chrono_rift {
inline constexpr char kSharedMemoryName[] = "/chrono_rift_game";
inline constexpr std::size_t kNameSize   = 48;
inline constexpr std::size_t kStatusSize = 80;
inline constexpr int kMaxPlayers      = 4;
inline constexpr int kMaxEnemies      = 9;
inline constexpr int kInventorySlots  = 20;
inline constexpr int kNumWeapons      = 8;
inline constexpr int kWeaponSolarCore    = 0;
inline constexpr int kWeaponLunarBlade   = 1;
inline constexpr int kWeaponIronHalberd  = 2;
inline constexpr int kWeaponVenomDagger  = 3;
inline constexpr int kWeaponThunderstaff = 4;
inline constexpr int kWeaponObsidianAxe  = 5;
inline constexpr int kWeaponFrostbow     = 6;
inline constexpr int kWeaponSplinterStick= 7;
inline constexpr int kNumArtifacts    = 3;
inline constexpr int kArtifactSolar   = 0;
inline constexpr int kArtifactLunar   = 1;
inline constexpr int kArtifactEclipse = 2;
inline constexpr int kStoredKind      = 99;
inline constexpr int kActionLogEntries= 20;
// ============================================================
// WeaponDef
// ============================================================
struct WeaponDef {
    char name[kNameSize];
    int  slots;
    int  damage;
};
// ============================================================
// EntityState
// ============================================================
struct EntityState {
    int  alive;
    int  hp;
    int  max_hp;
    int  stamina;
    int  max_stamina;
    int  speed;
    int  damage;
    int  stunned;
    int  actor_id;
    char name[kNameSize];
};
// ============================================================
// MoveBuffer
// ============================================================
struct MoveBuffer {
    int  ready;
    int  actor_kind;
    int  actor_index;
    int  action_type;
    int  target_index;
    int  damage;
    char note[kStatusSize];
};
inline constexpr int kActionStrike    = 1;
inline constexpr int kActionSkip      = 2;
inline constexpr int kActionUltimate  = 3;
inline constexpr int kActionStun      = 4;
inline constexpr int kActionPickup    = 5;
inline constexpr int kActionLock      = 6;
inline constexpr int kActionUnlock    = 7;
inline constexpr int kActionExhaust   = 8;
inline constexpr int kActionHeal      = 9;
inline constexpr int kActionUseWeapon = 10;
inline constexpr int kActionSwapIn    = 11;
// ============================================================
// ArtifactState
// ============================================================
struct ArtifactState {
    int  held_by_kind;
    int  held_by_index;
    int  waiting_kind;
    int  waiting_index;
    int  locked;
    int  lock_owner_kind;
    int  lock_owner_index;
    char name[kNameSize];
};
inline constexpr int kMaxStorage = 10;
struct StorageEntry {
    int weapon_index;
};
inline constexpr int kWeaponCont = -2;
// ============================================================
// SharedState
// ============================================================
struct SharedState {
    sem_t state_lock;
    sem_t arbiter_ready;
    sem_t hip_ready;
    sem_t asp_ready;
    sem_t enemy_stun_sem[kMaxEnemies];
    int running;
    int winner;
    int turn_actor_kind;
    int turn_actor_index;
    int active_players;
    int active_enemies;
    int enemies_killed;
    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;
    EntityState players[kMaxPlayers];
    EntityState enemies[kMaxEnemies];
    int enemy_weapons[kMaxEnemies];
    MoveBuffer move_buffer;
    int inventory_slots[kInventorySlots];
    StorageEntry storage[kMaxStorage];
    int          storage_count;
    WeaponDef weapons[kNumWeapons];
    ArtifactState artifacts[kNumArtifacts];
    int pending_drop_weapon;
    int pending_drop_enemy;
    int enemy_stunned[kMaxEnemies];
    char action_log[kActionLogEntries][kStatusSize];
    int  action_log_head;
    int  action_log_count;
    char arbiter_status[kStatusSize];
    char hip_status[kStatusSize];
    char asp_status[kStatusSize];
    char last_error[kStatusSize];
};
// ============================================================
// copy_text
// ============================================================
inline void copy_text(char* dst, size_t dst_size, const char* src) {
    snprintf(dst, dst_size, "%s", src);
}
// ============================================================
// sem_wait_interruptible
// ============================================================
inline bool sem_wait_interruptible(sem_t* s, volatile int& keep_running) {
    while (keep_running) {
        if (sem_wait(s) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
    return false;
}
// ============================================================
// init_weapon_catalogue
// ============================================================
inline void init_weapon_catalogue(WeaponDef* w) {
    copy_text(w[0].name, kNameSize, "Solar Core");    w[0].slots=10; w[0].damage=95;
    copy_text(w[1].name, kNameSize, "Lunar Blade");   w[1].slots=10; w[1].damage=90;
    copy_text(w[2].name, kNameSize, "Iron Halberd");  w[2].slots=7;  w[2].damage=55;
    copy_text(w[3].name, kNameSize, "Venom Dagger");  w[3].slots=4;  w[3].damage=30;
    copy_text(w[4].name, kNameSize, "Thunderstaff");  w[4].slots=6;  w[4].damage=50;
    copy_text(w[5].name, kNameSize, "Obsidian Axe");  w[5].slots=5;  w[5].damage=45;
    copy_text(w[6].name, kNameSize, "Frostbow");      w[6].slots=6;  w[6].damage=48;
    copy_text(w[7].name, kNameSize, "Splinter Stick");w[7].slots=2;  w[7].damage=12;
}
// ============================================================
// inventory_find_free
// ============================================================
inline int inventory_find_free(const int* inv, int slots) {
    int run = 0, run_start = -1;
    for (int i = 0; i < kInventorySlots; ++i) {
        if (inv[i] == -1) {
            if (run == 0) {
                run_start = i;
            }
            if (++run >= slots) {
                return run_start;
            }
        } else {
            run = 0; run_start = -1;
        }
    }
    return -1;
}
// ============================================================
// inventory_place
// ============================================================
inline void inventory_place(int* inv, int start, int w, int slots) {
    for (int i = 0; i < slots; ++i) {
        inv[start + i] = (i == 0) ? w : kWeaponCont;
    }
}
// ============================================================
// inventory_remove
// ============================================================
inline bool inventory_remove(int* inv, int w) {
    for (int i = 0; i < kInventorySlots; ++i) {
        if (inv[i] == w) {
            inv[i] = -1;
            for (int j = i+1; j < kInventorySlots && inv[j] == kWeaponCont; ++j) {
                inv[j] = -1;
            }
            return true;
        }
    }
    return false;
}
// ============================================================
// inventory_has
// ============================================================
inline bool inventory_has(const int* inv, int w) {
    for (int i = 0; i < kInventorySlots; ++i) {
        if (inv[i] == w) {
            return true;
        }
    }
    return false;
}
// ============================================================
// storage_add
// ============================================================
inline bool storage_add(StorageEntry* stor, int& count, int w) {
    if (count >= kMaxStorage) {
        return false;
    }
    stor[count++].weapon_index = w;
    return true;
}
// ============================================================
// storage_remove
// ============================================================
inline bool storage_remove(StorageEntry* stor, int& count, int w) {
    for (int i = 0; i < count; ++i) {
        if (stor[i].weapon_index == w) {
            stor[i] = stor[--count];
            stor[count].weapon_index = -1;
            return true;
        }
    }
    return false;
}
// ============================================================
// create_shared_state
// ============================================================
inline SharedState* create_shared_state(int& fd) {
    shm_unlink(kSharedMemoryName);
    fd = shm_open(kSharedMemoryName, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open create"); exit(EXIT_FAILURE);
    }
    if (ftruncate(fd, (off_t)sizeof(SharedState)) == -1) {
        perror("ftruncate"); close(fd); shm_unlink(kSharedMemoryName); exit(EXIT_FAILURE);
    }
    void* p = mmap(nullptr, sizeof(SharedState), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap"); close(fd); shm_unlink(kSharedMemoryName); exit(EXIT_FAILURE);
    }
    SharedState* s = static_cast<SharedState*>(p);
    memset(s, 0, sizeof(SharedState));
    if (sem_init(&s->state_lock,    1, 1) ||
        sem_init(&s->arbiter_ready, 1, 0) ||
        sem_init(&s->hip_ready,     1, 0) ||
        sem_init(&s->asp_ready,     1, 0)) {
        perror("sem_init core"); munmap(s, sizeof(SharedState));
        close(fd); shm_unlink(kSharedMemoryName); exit(EXIT_FAILURE);
    }
    for (int i = 0; i < kMaxEnemies; ++i) {
        if (sem_init(&s->enemy_stun_sem[i], 1, 0) != 0) {
            perror("sem_init stun"); munmap(s, sizeof(SharedState));
            close(fd); shm_unlink(kSharedMemoryName); exit(EXIT_FAILURE);
        }
    }
    s->running = 1; s->winner = -1;
    s->turn_actor_kind = -1; s->turn_actor_index = -1;
    s->pending_drop_weapon = -1; s->pending_drop_enemy = -1;
    s->storage_count = 0;
    for (int i = 0; i < kInventorySlots; ++i) {
        s->inventory_slots[i] = -1;
    }
    for (int i = 0; i < kMaxStorage; ++i) {
        s->storage[i].weapon_index = -1;
    }
    for (int i = 0; i < kMaxEnemies; ++i) {
        s->enemy_stunned[i] = 0;
        s->enemy_weapons[i] = -1;
    }
    for (int i = 0; i < kNumArtifacts; ++i) {
        s->artifacts[i].held_by_kind     = -2;
        s->artifacts[i].held_by_index    = -1;
        s->artifacts[i].waiting_kind     = -1;
        s->artifacts[i].waiting_index    = -1;
        s->artifacts[i].locked           =  0;
        s->artifacts[i].lock_owner_kind  = -1;
        s->artifacts[i].lock_owner_index = -1;
        s->artifacts[i].name[0]          = '\0';
    }
    init_weapon_catalogue(s->weapons);
    copy_text(s->last_error,     kStatusSize, "none");
    copy_text(s->arbiter_status, kStatusSize, "initialized");
    copy_text(s->action_log[0],  kStatusSize, "Game started");
    s->action_log_head = 1; s->action_log_count = 1;
    return s;
}
// ============================================================
// attach_shared_state
// ============================================================
inline SharedState* attach_shared_state(int& fd) {
    fd = -1;
    for (int i = 0; i < 100; ++i) {
        fd = shm_open(kSharedMemoryName, O_RDWR, 0666);
        if (fd != -1) {
            break;
        }
        usleep(100000);
    }
    if (fd == -1) {
        perror("shm_open attach"); exit(EXIT_FAILURE);
    }
    void* p = mmap(nullptr, sizeof(SharedState), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap attach"); close(fd); exit(EXIT_FAILURE);
    }
    return static_cast<SharedState*>(p);
}
// ============================================================
// detach_shared_state
// ============================================================
inline void detach_shared_state(SharedState* s, int fd) {
    if (s && s != MAP_FAILED) {
        munmap(s, sizeof(SharedState));
    }
    if (fd != -1) {
        close(fd);
    }
}
// ============================================================
// destroy_shared_state
// ============================================================
inline void destroy_shared_state(SharedState* s, int fd) {
    if (s && s != MAP_FAILED) {
        sem_destroy(&s->state_lock);
        sem_destroy(&s->arbiter_ready);
        sem_destroy(&s->hip_ready);
        sem_destroy(&s->asp_ready);
        for (int i = 0; i < kMaxEnemies; ++i) {
            sem_destroy(&s->enemy_stun_sem[i]);
        }
        munmap(s, sizeof(SharedState));
    }
    if (fd != -1) {
        close(fd);
    }
    shm_unlink(kSharedMemoryName);
}
} // namespace chrono_rift