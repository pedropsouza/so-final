#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define RESTAURANT_COUNT 5
#define MOTORCYCLE_COUNT RESTAURANT_COUNT
#define ORDER_CAP RESTAURANT_COUNT * 5
#define RIDER_COUNT RESTAURANT_COUNT * 2

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

#define ORDER_STATE_MAP(X)                                                     \
  X(ORDER_STATE_UNKNOWN, "unknown")                                            \
  X(ORDER_STATE_DELIVERED, "delivered")                                        \
  X(ORDER_STATE_PLACED, "placed")                                              \
  X(ORDER_STATE_WAIT_RIDER, "waiting rider")                                   \
  X(ORDER_STATE_WAIT_VEHICLE, "waiting vehicle")                               \
  X(ORDER_STATE_MOVING, "on the move")

#define STR_ASSIGNMENT_XTRACTOR(NUM, DESC) [NUM] = DESC,

const char *order_states[] = {ORDER_STATE_MAP(STR_ASSIGNMENT_XTRACTOR)};
#undef STR_ASSIGNMENT_XTRACTOR

struct rider_details {
  uint32_t order_rz, motorcycle_rz;
  pthread_t thread_handle;
};
struct rider_details riders[RIDER_COUNT] = {0};

// redundant info kept for debugging
uint32_t moto_riders_rz[MOTORCYCLE_COUNT] = {0};
uint32_t order_riders_rz[ORDER_CAP] = {0};

struct order_details {
  uint32_t restaurant;
  int state;
};
struct order_details orders[ORDER_CAP] = {0};
volatile uint32_t
    orders_needle; // shared counter will lead to TOCTOU, as expected
pthread_barrier_t setup_finished;

void veteran(uint32_t rider_id);
void newbie(uint32_t rider_id);
struct thread_args {
  uint32_t rider_id;
  void (*strategy)(uint32_t);
};
void *thread_procedure(void *args);

void initialize();
void finish();

struct order_details generate_order();
uint32_t finish_order(uint32_t order);
int lock_motorcycle(uint32_t rider);
int unlock_motorcycle(uint32_t rider);
int lock_order(uint32_t rider);
int unlock_order(uint32_t rider);

void dispatch();
uint32_t order_available();

int main(int argc, char **argv) {
  initialize();

  for (int i = 0; i < 300; i++) {
    dispatch();
    usleep(80000);
  }

  finish();
}

void initialize() {
  srand(70649);
  orders_needle = 0;
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
  }
  pthread_barrier_wait(&setup_finished);
}

void finish() {
  for (int i = 0; i < RIDER_COUNT; i++) {
    int retv = pthread_cancel(riders[i].thread_handle);
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

struct order_details generate_order() {
  return (struct order_details){
      .restaurant = (rand() % RESTAURANT_COUNT) + 1,
      .state = ORDER_STATE_PLACED,
  };
}

void dispatch() {
  orders[orders_needle] = generate_order();
  printf("dispatching order number %u for restaurant %s\n",
         orders_needle,
         restaurant_descs[orders[orders_needle].restaurant]);
  orders_needle = (orders_needle + 1);
  if (orders_needle >= ORDER_CAP) {
      orders_needle = 0;
  }
  // assuming the circular buffer is not over its
  // holding capacity, this test should always pass
  while ((orders[orders_needle].state >> 1)) { // the cheap comparison
      usleep(10000); // wait for the riders to finish some deliveries
  }
}

int lock_order(uint32_t rider_id) {
    uint32_t order_id = riders[rider_id].order_rz - 1;
    int heldby = order_riders_rz[order_id];

    int retv = pthread_mutex_trylock(order_mtxs + order_id);
    switch (retv) {
        case 0:
            fprintf(stdout, "rider %u locked order %u for restaurant %s\n",
                    rider_id, order_id,
                    restaurant_descs[orders[order_id].restaurant]);
            order_riders_rz[order_id] = rider_id + 1;
            break;
        case EBUSY:
            fprintf(stdout,
                    "rider %u is waiting to lock order %u for restaurant %s",
                    rider_id, order_id,
                    restaurant_descs[orders[order_id].restaurant]);

            fprintf(stdout,
                    (heldby != 0)? " (held by rider %u)\n" : " (raced)\n",
                    heldby - 1);
            retv = pthread_mutex_lock(order_mtxs + order_id);
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
    int heldby = moto_riders_rz[moto_id];

    int retv = pthread_mutex_trylock(moto_mtxs + moto_id);
    switch (retv) {
        case 0:
            fprintf(stdout, "rider %u locked motorcycle %u for restaurant %s\n", rider_id, moto_id, restaurant_descs[orders[order_id].restaurant]);
            moto_riders_rz[moto_id] = rider_id + 1;
            break;
        case EBUSY:
            fprintf(stdout,
                    "rider %u is waiting to lock motorcycle %u for restaurant %s",
                    rider_id, moto_id,
                    restaurant_descs[orders[order_id].restaurant]);
            fprintf(stdout,
                    (heldby != 0)? " (held by rider %u)\n" : " (raced)\n",
                    heldby - 1);
            retv = pthread_mutex_lock(moto_mtxs + moto_id);
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
            fprintf(stdout, "rider %u unlocked order %u for %s\n", rider_id, order_id, restaurant_descs[orders[order_id].restaurant]);
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
            fprintf(stdout, "rider %u unlocked motorcycle %u for %s\n", rider_id, moto_id, restaurant_descs[orders[order_id].restaurant]);
            moto_riders_rz[moto_id] = 0;
            break;
        default:
            assert(0);
            break;
    }
    return retv;
}

uint32_t order_available() {
  usleep(10000 + (rand() % 1000)); // simulate check cost
  uint32_t o_id = orders_needle - 1;
  if (o_id > ORDER_CAP) {
    o_id = ORDER_CAP - 1;
  }
  return (orders[o_id].state == ORDER_STATE_PLACED) * (o_id + 1);
}

void *thread_procedure(void *args_) {
    struct thread_args args = *(struct thread_args *)args_;
    pthread_barrier_wait(&setup_finished);
    while (1) { args.strategy(args.rider_id); }
    return NULL;
}

void veteran(uint32_t rider_id) {
  uint32_t order_id = 0;
  while (!order_id) {
      order_id = order_available();
  }
  order_id--; // bring it back from rz domain

  // link the order to our rider
  riders[rider_id] = (struct rider_details) {
      .order_rz = order_id + 1,
      .motorcycle_rz = orders[order_id].restaurant - 1,
  };
  int retv;
  retv = lock_motorcycle(rider_id);
  if (retv != 0) { goto busyloop; }
  orders[order_id].state = ORDER_STATE_WAIT_RIDER;
  usleep(10000); // walk to the order
  retv = lock_order(rider_id);
  if (retv != 0) { goto busyloop; }
  orders[order_id].state = ORDER_STATE_MOVING;

  usleep(800000); // deliver the order
  orders[order_id].state = ORDER_STATE_DELIVERED;

  retv = unlock_motorcycle(rider_id);
  if (retv != 0) { goto busyloop; }
  retv = unlock_order(rider_id);
  if (retv != 0) { goto busyloop; }

  return;

  busyloop: while (1);
}

void newbie(uint32_t rider_id) {
  uint32_t order_id = 0;
  while (!order_id) {
      order_id = order_available();
  }
  order_id--; // bring it back from rz domain

  // link the order to our rider
  riders[rider_id] = (struct rider_details) {
      .order_rz = order_id + 1,
      .motorcycle_rz = orders[order_id].restaurant - 1,
  };
  int retv;
  retv = lock_order(rider_id);
  if (retv != 0) { goto busyloop; }
  orders[order_id].state = ORDER_STATE_WAIT_VEHICLE;

  usleep(10000); // walk to the motorcycle
  retv = lock_motorcycle(rider_id);
  if (retv != 0) { goto busyloop; }
  orders[order_id].state = ORDER_STATE_MOVING;

  usleep(800000); // deliver the order
  orders[order_id].state = ORDER_STATE_DELIVERED;

  retv = unlock_motorcycle(rider_id);
  if (retv != 0) { goto busyloop; }
  retv = unlock_order(rider_id);
  if (retv != 0) { goto busyloop; }

  return;

  busyloop: while (1);
}

