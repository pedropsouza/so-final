#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>

#define RESTAURANT_COUNT 5
#define MOTORCYCLE_COUNT RESTAURANT_COUNT
#define ORDER_CAP RESTAURANT_COUNT * 25
#define RIDER_COUNT RESTAURANT_COUNT * 2

pthread_mutex_t
  moto_mtxs[MOTORCYCLE_COUNT+1], // 1-to-1 with biz, so we index by business
  order_mtxs[ORDER_CAP];

const char *restaurant_descs[RESTAURANT_COUNT+1] = {
[0] = "INVALID", // this way zeroed-out order details stand out
[1] = "Bóia Tropical",
[2] = "Troppo Bene",
[3] = "Junto com Frango",
[4] = "Abduzidu's Lanches",
[5] = "Bom Buffet",
};

#define ORDER_STATE_UNKNOWN 0
#define ORDER_STATE_PLACED 1
#define ORDER_STATE_WAIT_RIDER 2
#define ORDER_STATE_WAIT_VEHICLE 3
#define ORDER_STATE_DELIVERED 4

#define ORDER_STATE_MAP(X) \
    X(ORDER_STATE_UNKNOWN, "unknown") \
    X(ORDER_STATE_PLACED, "placed") \
    X(ORDER_STATE_WAIT_RIDER, "waiting rider") \
    X(ORDER_STATE_WAIT_VEHICLE, "waiting vehicle") \
    X(ORDER_STATE_DELIVERED, "delivered")

#define IDX_ASSIGNMENT_XTRACTOR(NUM, DESC) \
    [NUM] = DESC,

const char *order_states[] = {
  ORDER_STATE_MAP(IDX_ASSIGNMENT_XTRACTOR)
};

// redundant info kept for debugging
uint32_t moto_riders[MOTORCYCLE_COUNT] = {0};
uint32_t order_riders[MOTORCYCLE_COUNT] = {0};

struct rider_details {
    uint32_t order, motorcycle;
};
struct rider_details riders[RIDER_COUNT];

struct order_details {
    uint32_t restaurant;
    char *state;
};
struct order_details orders[ORDER_CAP];


uint32_t veteran();
uint32_t newbie();
void initialize();
void finish();

uint32_t generate_order();
uint32_t finish_order(uint32_t order);
int lock_motorcycle(uint32_t rider);
int unlock_motorcycle(uint32_t rider);
int lock_order(uint32_t rider);
int unlock_order(uint32_t rider);

void dispatch();

int main(int argc, char **argv) {
    initialize();

    for (int i = 0; i < 100; i++)
        dispatch();
    
    finish();
}

