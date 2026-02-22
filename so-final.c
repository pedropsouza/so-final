#include <assert.h>
#include <bits/pthreadtypes.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#define RESTAURANT_COUNT 5
#define MOTORCYCLE_COUNT RESTAURANT_COUNT
#define ORDER_CAP (RESTAURANT_COUNT*2)
#define RIDER_COUNT RESTAURANT_COUNT * 4

pthread_mutex_t moto_mtxs[MOTORCYCLE_COUNT], // 1-to-1 with biz, so we index by
                                             // (business - 1)
    order_mtxs[ORDER_CAP];

const char *restaurant_descs[RESTAURANT_COUNT + 1] = {
    [0] = "INVALID", // this way zeroed-out order details stand out
    [1] = "Bóia Tropical",
    [2] = "Troppo Bene",
    [3] = "Juntos, Como o Frango",
    [4] = "Abduzidu's Lanches",
    [5] = "Bom Buffet",
};

// unknown and delivered must be 0th and 1st idx, for cheap empty
// checks @ dispatch()
#define ORDER_STATE_UNKNOWN 0
#define ORDER_STATE_DELIVERED 1
#define ORDER_STATE_PLACED 2
#define ORDER_STATE_WAIT_RIDER 3
#define ORDER_STATE_WAIT_VEHICLE 4
#define ORDER_STATE_MOVING 5

#define CHECK_TIME_US 250
#define WAIT_MOTO_TIME_US 1000
#define WAIT_ORDER_TIME_US 1000
#define WALK_TIME_US 1000
#define DELIVERY_TIME_US 80000
#define MAIN_LOOP_TIME_US 20000

#define ORDER_STATE_MAP(X)                               \
  X(ORDER_STATE_UNKNOWN,      "unknown",         "....") \
  X(ORDER_STATE_DELIVERED,    "delivered",       "DONE") \
  X(ORDER_STATE_PLACED,       "placed",          "WAIT") \
  X(ORDER_STATE_WAIT_RIDER,   "waiting rider",   "PICK") \
  X(ORDER_STATE_WAIT_VEHICLE, "waiting vehicle", "VHCL") \
  X(ORDER_STATE_MOVING,       "on the move",     "TRVL")

#define STR_ASSIGNMENT_XTRACTOR(NUM, DESC, SHRT) [NUM] = DESC,
const char *order_states[] = {ORDER_STATE_MAP(STR_ASSIGNMENT_XTRACTOR)};
#undef STR_ASSIGNMENT_XTRACTOR

#define SHORTHAND_ASSIGNMENT_XTRACTOR(NUM, DESC, SHRT) [NUM] = SHRT,
const char *order_state_shorthands[] = {ORDER_STATE_MAP(SHORTHAND_ASSIGNMENT_XTRACTOR)};
#undef SHORTHAND_ASSIGNMENT_XTRACTOR

struct rider_details {
  uint32_t order_rz, motorcycle_rz;
  pthread_t thread_handle;
  char log_header[32];
};
struct rider_details riders[RIDER_COUNT] = {0};

// redundant info kept for debugging
uint32_t moto_riders_rz[MOTORCYCLE_COUNT] = {0};
uint32_t order_riders_rz[ORDER_CAP] = {0};

struct order_details {
  uint32_t uid, restaurant;
  int state;
};
struct order_details orders[ORDER_CAP] = {0};
volatile uint32_t
    orders_head,
    orders_tail; // shared counters will lead to TOCTOU, as expected
pthread_barrier_t setup_finished;

void veteran(uint32_t rider_id);
void newbie(uint32_t rider_id);
struct thread_args {
  uint32_t rider_id;
  void (*strategy)(uint32_t);
};
void *thread_procedure(void *args);
void rider_log(uint32_t rider_id, const char *fmt, ...);
// unhigienic macro, expects rider_id to in the lexical scope
#define LOG(...) rider_log(rider_id, __VA_ARGS__)


void initialize();
void finish();
volatile char finished_bool = 0;

struct order_details generate_order(uint32_t uid);
uint32_t finish_order(uint32_t order);
int lock_motorcycle(uint32_t rider);
int unlock_motorcycle(uint32_t rider);
int lock_order(uint32_t rider);
int unlock_order(uint32_t rider);

// returns 1 if successful
int dispatch(uint32_t uid);
uint32_t order_available();

int main(int argc, char **argv) {
  initialize();

  for (int i = 0; i < 30;) {
    i += dispatch(i);
    printf("\r %4u:%4u [", orders_head % ORDER_CAP, orders_tail % ORDER_CAP);
    for (int i = 0; i < ORDER_CAP; i++) {
        char order_desc[32];
        int order_valid = orders[i].state != ORDER_STATE_UNKNOWN;
        int order_claimed = !(0
            || orders[i].state == ORDER_STATE_PLACED
            || orders[i].state == ORDER_STATE_UNKNOWN
            || orders[i].state == ORDER_STATE_DELIVERED
            || 0);
        int off = snprintf(order_desc, sizeof(order_desc),
                 order_valid? "(%4u:" : "(----:",
                 orders[i].uid);
        snprintf(order_desc + off, sizeof(order_desc) - off,
                 order_claimed? "%4u)" : "----)",
                 order_riders_rz[i] - 1);
        printf((i == ORDER_CAP - 1)? "%s %s" : "%s %s, ",
               order_state_shorthands[orders[i].state],
               order_desc);
    }
    printf("]");
    fflush(stdout);
    usleep(MAIN_LOOP_TIME_US);
  }

  int orders_finished = 0;
  for (int i = 0; 1; i++) {
      if (i >= ORDER_CAP) { i = 0; }
      if (orders[i].state == ORDER_STATE_DELIVERED || orders[i].state == ORDER_STATE_UNKNOWN) {
          orders_finished++;
          if (orders_finished >= ORDER_CAP) {
              break; // all done
          }
      } else {
          i--;
          orders_finished = 0;
          usleep(MAIN_LOOP_TIME_US);
          continue;
      }
  }

  printf("\ndone, cleaning up...\n");
  finish();
}

void initialize() {
  srand(70649);
  orders_head = 0;
  orders_tail = 0;
  int retv = pthread_barrier_init(&setup_finished, NULL, RIDER_COUNT + 1);
  assert(retv == 0);

  for (int i = 0; i < MOTORCYCLE_COUNT; i++) {
    int retv = pthread_mutex_init(moto_mtxs + i, NULL);
    assert(retv == 0);
  }
  for (int i = 0; i < ORDER_CAP; i++) {
    int retv = pthread_mutex_init(order_mtxs + i, NULL);
    assert(retv == 0);
  }

  struct thread_args big_buffer_of_args[RIDER_COUNT];
  for (int i = 0; i < RIDER_COUNT; i++) {
    big_buffer_of_args[i] = (struct thread_args){
        .rider_id = i,
        .strategy = (i < RIDER_COUNT / 2) ? veteran : newbie,
    };
    pthread_t thread_handle;
    int retv = pthread_create(&thread_handle, NULL, thread_procedure, (void *)(big_buffer_of_args + i));
    assert(retv == 0);

    riders[i] = (struct rider_details){
        .order_rz = 0,
        .motorcycle_rz = 0,
        .thread_handle = thread_handle,
    };
    snprintf((char *)(&riders[i].log_header),
             sizeof(riders[i].log_header),
             "[rider %4u (%s)]:",
             i,
             (big_buffer_of_args[i].strategy == veteran)?
             "veteran" : "newbie");
  }
  pthread_barrier_wait(&setup_finished);
}

void finish() {
  finished_bool = 1;
  for (int i = 0; i < RIDER_COUNT; i++) {
    int retv = pthread_join(riders[i].thread_handle, NULL);
    assert(retv == 0);
  }
  for (int i = 0; i < MOTORCYCLE_COUNT; i++) {
    int retv = pthread_mutex_destroy(moto_mtxs + i);
    assert(retv == 0);
  }
  for (int i = 0; i < ORDER_CAP; i++) {
    int retv = pthread_mutex_destroy(order_mtxs + i);
    assert(retv == 0);
  }
}

struct order_details generate_order(uint32_t uid) {
  return (struct order_details){
      .uid = uid,
      .restaurant = (rand() % RESTAURANT_COUNT) + 1,
      .state = ORDER_STATE_PLACED,
  };
}

int dispatch(uint32_t uid) {
  // circular queue scheme:
  // leftmost  idx: head (H)
  // rightmost idx: tail (L)
  // both start @ 0
  // possible situations:
  // (HL)....    # both in the same pos, low demand
  // (H)____(L)  # far apart without wraparound, high demand
  // __(L).(H)_  # far apart with wraparound
  // new orders go on the tail,
  // riders take orders from the span between head and tail

  // always try to bring the head closer to the tail
  if (orders[orders_head % ORDER_CAP].state == ORDER_STATE_DELIVERED) {
      orders_head++;
  }

  // queue capacity check
  // assuming the circular buffer is not over its
  // holding capacity, this test should always pass
  if (orders[orders_tail % ORDER_CAP].state >> 1) { // the cheap comparison
      printf("\ndispatch waiting on queue position %u", orders_tail % ORDER_CAP);
      usleep(MAIN_LOOP_TIME_US); // wait for the riders to finish some deliveries
      return 0; // hasn't dispatched a new order successfully
  }
  orders[orders_tail % ORDER_CAP] = generate_order(uid);
  printf("\ndispatching order number %u (queue pos %u) for restaurant %s\n",
         uid,
         orders_tail % ORDER_CAP,
         restaurant_descs[orders[orders_tail % ORDER_CAP].restaurant]);
  orders_tail++;
  return 1;
}

int lock_order(uint32_t rider_id) {
    uint32_t order_id = riders[rider_id].order_rz - 1;
    int retv = pthread_mutex_trylock(order_mtxs + order_id);
    switch (retv) {
        case 0:
          LOG("locked order %u for restaurant %s",
              orders[order_id].uid,
              restaurant_descs[orders[order_id].restaurant]);
          order_riders_rz[order_id] = rider_id + 1;
          break;
        case EBUSY:
            LOG("waiting to lock order %u for restaurant %s",
                orders[order_id].uid,
                restaurant_descs[orders[order_id].restaurant]);
            break;
        default:
            assert(0);
            break;
    }
    return retv;
}

int lock_motorcycle(uint32_t rider_id) {
    uint32_t order_id = riders[rider_id].order_rz - 1;
    uint32_t moto_id = orders[order_id].restaurant - 1;
    int retv = pthread_mutex_trylock(moto_mtxs + moto_id);
    switch (retv) {
        case 0:
            LOG("locked motorcycle %u for restaurant %s",
                moto_id, restaurant_descs[orders[order_id].restaurant]);
            moto_riders_rz[moto_id] = rider_id + 1;
            break;
        case EBUSY:
          LOG("waiting to lock motorcycle %u for restaurant %s",
              moto_id, restaurant_descs[orders[order_id].restaurant]);
          break;
        default:
            assert(0);
            break;
    }
    return retv;
}

int unlock_order(uint32_t rider_id) {
    uint32_t order_id = riders[rider_id].order_rz - 1;
    int retv = pthread_mutex_unlock(order_mtxs + order_id);
    switch (retv) {
        case 0:
            LOG("unlocked order %u for %s",
                orders[order_id].uid, restaurant_descs[orders[order_id].restaurant]);
            order_riders_rz[order_id] = 0;
            break;
        default:
            assert(0);
            break;
    }
    return retv;
}

int unlock_motorcycle(uint32_t rider_id) {
    uint32_t order_id = riders[rider_id].order_rz - 1;
    uint32_t moto_id = orders[order_id].restaurant - 1;
    int retv = pthread_mutex_unlock(moto_mtxs + moto_id);
    switch (retv) {
        case 0:
            LOG("unlocked motorcycle %u for %s",
                moto_id, restaurant_descs[orders[order_id].restaurant]);
            moto_riders_rz[moto_id] = 0;
            break;
        default:
            assert(0);
            break;
    }
    return retv;
}

#define ABS(V) (((V) < 0)? -(V) : (V))
// returns available order id + 1 or 0 if none available
uint32_t order_available() {
  usleep(CHECK_TIME_US); // simulate check cost
  int32_t h = orders_head, t = orders_tail;
  int32_t span_width = (t - h);
  if (span_width > ORDER_CAP) {
      span_width = ORDER_CAP;
  }
  int roffset = (span_width > 0)? random() % span_width : 0;
  
  for (int i = 0; i <= span_width; i++) {
    const uint32_t j = (span_width != 0)? (h + roffset + i) % span_width : 0;
    const uint32_t needle = (h + j) % ORDER_CAP;
    uint32_t retv = (orders[needle].state == ORDER_STATE_PLACED) * (needle + 1);
    if (retv != 0) return retv;
  }
  return 0;
}

void *thread_procedure(void *args_) {
    struct thread_args args = *(struct thread_args *)args_;
    pthread_barrier_wait(&setup_finished);
    while (!finished_bool) { args.strategy(args.rider_id); }
    return NULL;
}

void rider_log(uint32_t rider_id, const char *fmt, ...) {
    // since fmtbuf can be raced, we oughta lock on logging
    // (and i'm not really sure using thread local static buffers is a good idea)
    static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
    static char fmtbuf[256] = {0};

    int retv;
    retv = pthread_mutex_lock(&log_mtx);
    assert(retv == 0);
    int off = snprintf(fmtbuf, sizeof(fmtbuf), "\n%s %s", riders[rider_id].log_header, fmt);
    va_list varargs;
    va_start(varargs, fmt);
    vfprintf(stdout, fmtbuf, varargs);
    pthread_mutex_unlock(&log_mtx);
    va_end(varargs);
    fflush(stdout);
}

void veteran(uint32_t rider_id) {
  uint32_t order_id = 0;
  while (!order_id && !finished_bool) {
    order_id = order_available();
  }
  if (finished_bool) return;
  order_id--; // bring it back from rz domain

  // link the order to our rider
  riders[rider_id].order_rz = order_id + 1;
  riders[rider_id].motorcycle_rz = orders[order_id].restaurant - 1;
  int retv;
  while (1) {
    retv = lock_motorcycle(rider_id);
    if (retv == 0) {
      break;
    } else if (orders[order_id].state != ORDER_STATE_PLACED) {
      LOG("gave up on order %u", orders[order_id].uid);
      return;
    }
    usleep(WAIT_MOTO_TIME_US);
  }
  orders[order_id].state = ORDER_STATE_WAIT_RIDER;

  usleep(WALK_TIME_US); // walk to the order
  while (1) {
    retv = lock_order(rider_id);
    if (retv == 0) {
      break;
    } else if (orders[order_id].state != ORDER_STATE_WAIT_RIDER) {
      LOG("detected a deadlock for their order %u. giving up the keys...",
          orders[order_id].uid);

      unlock_motorcycle(rider_id);
      return; // forfeit this order
    }
    usleep(WAIT_ORDER_TIME_US);
  }
  LOG("off to deliver order %u for restaurant %s",
      orders[order_id].uid, restaurant_descs[orders[order_id].restaurant]);
  orders[order_id].state = ORDER_STATE_MOVING;

  usleep(DELIVERY_TIME_US); // deliver the order
  orders[order_id].state = ORDER_STATE_DELIVERED;

  LOG("delivered order %u for restaurant %s",
      orders[order_id].uid,
      restaurant_descs[orders[order_id].restaurant]);
  retv = unlock_motorcycle(rider_id);
  if (retv != 0) {
    goto err;
  }
  retv = unlock_order(rider_id);
  if (retv != 0) {
    goto err;
  }

  // small rest to let the others take a turn
  usleep(500);
  return;
  err: LOG("errored out. busy looping!");
  while(1);
}

void newbie(uint32_t rider_id) {
  uint32_t order_id = 0;
  while (!order_id && !finished_bool) {
    order_id = order_available();
  }
  if (finished_bool) return;
  order_id--; // bring it back from rz domain

  // link the order to our rider
  riders[rider_id].order_rz = order_id + 1;
  riders[rider_id].motorcycle_rz = orders[order_id].restaurant - 1;
  int retv;
  while (1) {
    retv = lock_order(rider_id);
    if (retv == 0) {
      break;
    } else if (orders[order_id].state != ORDER_STATE_PLACED) {
      LOG("gave up on order %u", orders[order_id].uid);
      return;
    }
    usleep(WAIT_MOTO_TIME_US);
  }
  orders[order_id].state = ORDER_STATE_WAIT_VEHICLE;

  usleep(WALK_TIME_US); // walk to the motorcycle
  while (1) {
    retv = lock_motorcycle(rider_id);
    if (retv == 0) {
      break;
    } else if (orders[order_id].state != ORDER_STATE_WAIT_VEHICLE) {
      LOG("detected a deadlock for their order %u. giving up the order...",
          orders[order_id].uid);

      unlock_order(rider_id);
      return; // forfeit this order
    }
    usleep(WAIT_ORDER_TIME_US);
  }
  LOG("off to deliver order %u for restaurant %s",
      orders[order_id].uid, restaurant_descs[orders[order_id].restaurant]);
  orders[order_id].state = ORDER_STATE_MOVING;

  usleep(DELIVERY_TIME_US); // deliver the order
  orders[order_id].state = ORDER_STATE_DELIVERED;

  LOG("delivered order %u for restaurant %s",
      orders[order_id].uid,
      restaurant_descs[orders[order_id].restaurant]);
  retv = unlock_motorcycle(rider_id);
  if (retv != 0) {
    goto err;
  }
  retv = unlock_order(rider_id);
  if (retv != 0) {
    goto err;
  }

  // small rest to let the others take a turn
  usleep(500);
  return;
  err: LOG("errored out. busy looping!");
  while(1);
}
