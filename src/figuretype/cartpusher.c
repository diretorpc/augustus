#include "cartpusher.h"

#include "assets/assets.h"
#include "building/barracks.h"
#include "building/granary.h"
#include "building/industry.h"
#include "building/monument.h"
#include "building/storage.h"
#include "building/warehouse.h"
#include "city/health.h"
#include "city/map.h"
#include "city/resource.h"
#include "core/calc.h"
#include "core/config.h"
#include "core/image.h"
#include "figure/combat.h"
#include "figure/image.h"
#include "figure/movement.h"
#include "figure/route.h"
#include "game/resource.h"
#include "map/road_network.h"
#include "map/routing_terrain.h"
#include "map/terrain.h"

#define NON_STORABLE_RESOURCE_CARTPUSHER_MAX_WAIT_TICKS 300
#define VALID_MONUMENT_RECHECK_TICKS 60

static int cartpusher_carries_food(figure *f)
{
    return resource_is_food(f->resource_id);
}

static void set_cart_graphic(figure *f, int always_carries_resource)
{
    int carried = f->loads_sold_or_carrying == 0 ? always_carries_resource : f->loads_sold_or_carrying;
    if (carried == 0 || f->resource_id == RESOURCE_NONE) {
        f->cart_image_id = image_group(GROUP_FIGURE_CARTPUSHER_CART);
    } else if (carried == 1) {
        f->cart_image_id = resource_get_data(f->resource_id)->image.cart.single_load;
    } else if (cartpusher_carries_food(f) && carried >= 8) {
        f->cart_image_id = resource_get_data(f->resource_id)->image.cart.eight_loads;
    } else {
        f->cart_image_id = resource_get_data(f->resource_id)->image.cart.multiple_loads;
    }
}

static int should_change_destination(const figure *f, int building_id, int x_dst, int y_dst)
{
    if (!f->destination_building_id) {
        return 1;
    }
    building *current_destination = building_get(f->destination_building_id);
    // Same building
    if (f->destination_building_id == building_id && f->destination_x == x_dst && f->destination_y == y_dst &&
        current_destination->type == building_get(building_id)->type) {
        return 0;
    }
    switch (f->action_state) {
        case FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE:
            if (!building_warehouse_accepts_storage(current_destination, f->resource_id, 0)) {
                return 1;
            }
            break;
        case FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY:
            if (!building_granary_accepts_storage(current_destination, f->resource_id, 0)) {
                return 1;
            }
            break;
        case FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE:
            if (current_destination->type != BUILDING_WAREHOUSE &&
                current_destination->type != BUILDING_WAREHOUSE_SPACE &&
                current_destination->type != BUILDING_GRANARY) {
                building *new_destination = building_get(building_id);
                return current_destination->type == new_destination->type &&
                    new_destination->resources[f->resource_id] < current_destination->resources[f->resource_id];
            }
            if (current_destination->type == BUILDING_GRANARY) {
                if (!building_granary_accepts_storage(current_destination, f->resource_id, 0)) {
                    return 1;
                }
            } else if (!building_warehouse_accepts_storage(current_destination, f->resource_id, 0)) {
                return 1;
            }
            break;
        case FIGURE_ACTION_54_WAREHOUSEMAN_GETTING_FOOD:
            if (building_granary_amount_can_get_from(current_destination, building_get(f->building_id)) == 0) {
                return 1;
            }
            break;
        case FIGURE_ACTION_57_WAREHOUSEMAN_GETTING_RESOURCE:
            if (building_warehouse_amount_can_get_from(current_destination, f->collecting_item_id) == 0) {
                return 1;
            }
            break;
        default:
            return 0;
    }
    int distance_current = calc_maximum_distance(current_destination->x, current_destination->y, f->x, f->y);
    int distance_new = calc_maximum_distance(x_dst, y_dst, f->x, f->y);
    return distance_current / 2 > distance_new;
}

static void validate_action_for_old_destination(figure *f)
{
    if (f->type == FIGURE_CART_PUSHER) {
        building *b = building_get(f->destination_building_id);
        switch (f->action_state) {
            case FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE:
            case FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY:
            case FIGURE_ACTION_23_CARTPUSHER_DELIVERING_TO_WORKSHOP:
                if (building_is_workshop(b->type)) {
                    f->action_state = FIGURE_ACTION_23_CARTPUSHER_DELIVERING_TO_WORKSHOP;
                } else if (b->type == BUILDING_GRANARY) {
                    f->action_state = FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY;
                } else {
                    f->action_state = FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE;
                }
                break;
            default:
                break;
        }
    }
}

static void set_destination(figure *f, int action, int building_id, int x_dst, int y_dst)
{
    f->action_state = action;
    f->wait_ticks = 0;
    if (should_change_destination(f, building_id, x_dst, y_dst)) {
        figure_route_remove(f);
        f->destination_building_id = building_id;
        f->destination_x = x_dst;
        f->destination_y = y_dst;
    } else {
        validate_action_for_old_destination(f);
    }
}

static void determine_cartpusher_destination(figure *f, building *b, int road_network_id)
{
    map_point dst = { 0, 0 };
    int understaffed_storages = 0;

    int dst_building_id = 0;
    int is_storable = resource_is_storable(b->output_resource_id);
    // priority 1: warehouse if resource is on stockpile
    if (is_storable && (city_resource_is_stockpiled(b->output_resource_id) || building_stockpiling_enabled(b))) {
        dst_building_id = building_warehouse_for_storing(0, f->x, f->y,
                            b->output_resource_id, road_network_id, &understaffed_storages, &dst);
    }
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE, dst_building_id, dst.x, dst.y);
        return;
    }
    // priority 2: accepting granary for food
    dst_building_id = building_granary_for_storing(f->x, f->y,
        b->output_resource_id, road_network_id, 0, &understaffed_storages, &dst);
    if (config_get(CONFIG_GP_CH_FARMS_DELIVER_CLOSE)) {
        int dist = 0;
        building *src_building = building_get(f->building_id);
        building *dst_building = building_get(dst_building_id);
        int src_building_type = src_building->type;
        if ((src_building_type >= BUILDING_WHEAT_FARM && src_building_type <= BUILDING_PIG_FARM) || src_building_type == BUILDING_WHARF) {
            dist = calc_maximum_distance(src_building->x, src_building->y, dst_building->x, dst_building->y);
        }
        if (dist >= 64) {
            dst_building_id = 0;
        }
    }
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY, dst_building_id, dst.x, dst.y);
        return;
    }
    // priority 3: workshop for raw material
    dst_building_id = building_get_workshop_for_raw_material_with_room(f->x, f->y,
        b->output_resource_id, road_network_id, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_23_CARTPUSHER_DELIVERING_TO_WORKSHOP, dst_building_id, dst.x, dst.y);
        return;
    }
    if (!is_storable) {
        // Special priority for non-storable resource: monument under construction
        dst_building_id = building_monument_get_monument(f->x, f->y, b->output_resource_id, road_network_id, &dst);
        if (dst_building_id) {
            f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
            set_destination(f, FIGURE_ACTION_246_CARTPUSHER_DELIVERING_TO_MONUMENT, dst_building_id, dst.x, dst.y);
            building_monument_add_delivery(dst_building_id, f->id, b->output_resource_id, 1);
        } else {
            f->action_state = FIGURE_ACTION_245_CARTPUSHER_WAITING_FOR_DESTINATION;
        }
        return;
    }
    // priority 4: warehouse
    dst_building_id = building_warehouse_for_storing(0, f->x, f->y,
        b->output_resource_id, road_network_id, &understaffed_storages, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE, dst_building_id, dst.x, dst.y);
        return;
    }
    // priority 5: granary forced when on stockpile
    dst_building_id = building_granary_for_storing(f->x, f->y,
        b->output_resource_id, road_network_id, 1, &understaffed_storages, &dst);
    if (config_get(CONFIG_GP_CH_FARMS_DELIVER_CLOSE)) {
        int dist = 0;
        building *src_building = building_get(f->building_id);
        building *dst_building = building_get(dst_building_id);
        int src_building_type = src_building->type;
        if ((src_building_type >= BUILDING_WHEAT_FARM && src_building_type <= BUILDING_PIG_FARM) || src_building_type == BUILDING_WHARF) {
            dist = calc_maximum_distance(src_building->x, src_building->y, dst_building->x, dst_building->y);
        }
        if (dist >= 64) {
            dst_building_id = 0;
        }
    }
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY, dst_building_id, dst.x, dst.y);
        return;
    }
    // no one will accept
    f->wait_ticks = 0;
    // set cartpusher text
    f->min_max_seen = understaffed_storages ? 2 : 1;
}

static void determine_cartpusher_destination_food(figure *f, int road_network_id)
{
    building *b = building_get(f->building_id);
    map_point dst;
    // priority 1: accepting granary for food
    int dst_building_id = building_granary_for_storing(f->x, f->y,
        b->output_resource_id, road_network_id, 0, 0, &dst);
    if (dst_building_id && config_get(CONFIG_GP_CH_FARMS_DELIVER_CLOSE)) {
        int dist = 0;
        building *dst_building = building_get(dst_building_id);
        if ((b->type >= BUILDING_WHEAT_FARM && b->type <= BUILDING_PIG_FARM) || b->type == BUILDING_WHARF) {
            dist = calc_maximum_distance(b->x, b->y, dst_building->x, dst_building->y);
        }
        if (dist >= 64) {
            dst_building_id = 0;
        }
    }
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY, dst_building_id, dst.x, dst.y);
        return;
    }
    // priority 2: warehouse
    dst_building_id = building_warehouse_for_storing(0, f->x, f->y,
        b->output_resource_id, road_network_id, 0, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE, dst_building_id, dst.x, dst.y);
        return;
    }
    // priority 3: granary
    dst_building_id = building_granary_for_storing(f->x, f->y, b->output_resource_id, road_network_id, 1, 0, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY, dst_building_id, dst.x, dst.y);
        return;
    }
    // no one will accept, stand idle
    f->wait_ticks = 0;
}

static void update_image(figure *f)
{
    int dir = figure_image_normalize_direction(
        f->direction < 8 ? f->direction : f->previous_tile_direction);

    if (building_get(f->building_id)->type == BUILDING_ARMOURY) {
        if (f->action_state == FIGURE_ACTION_149_CORPSE) {
            f->image_id = assets_get_image_id("Walkers", "barracks_worker_death_01") + figure_image_corpse_offset(f);
        } else {
            f->image_id = assets_get_image_id("Walkers", "barracks_worker_ne_01") + dir * 12 + f->image_offset;
        }
    } else {
        int base_group = f->type == FIGURE_CART_PUSHER ? GROUP_FIGURE_CARTPUSHER : GROUP_FIGURE_MIGRANT;

        if (f->action_state == FIGURE_ACTION_149_CORPSE) {
            f->image_id = image_group(base_group) + figure_image_corpse_offset(f) + 96;
            f->cart_image_id = 0;
        } else {
            f->image_id = image_group(base_group) + dir + 8 * f->image_offset;
        }
    }
    if (f->cart_image_id) {
        f->cart_image_id += dir;
        figure_image_set_cart_offset(f, dir);
        if (f->loads_sold_or_carrying >= 8 && cartpusher_carries_food(f)) {
            f->y_offset_cart -= 40;
        }
    }
}

static int cartpusher_percentage_speed(figure *f)
{
    // Ceres grand temple base bonus
    building *src_building = building_get(f->building_id);
    int src_building_type = src_building->type;
    if (src_building_type >= BUILDING_WHEAT_FARM && src_building_type <= BUILDING_PIG_FARM) {
        if (building_monument_working(BUILDING_GRAND_TEMPLE_CERES)) {
            return 50;
        }
    }
    return 0;
}

static void reroute_cartpusher(figure *f)
{
    figure_route_remove(f);
    if (!map_routing_citizen_is_passable_terrain(f->grid_offset)) {
        f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
    }
    f->wait_ticks = 0;
}

void figure_cartpusher_action(figure *f)
{
    figure_image_increase_offset(f, 12);
    f->cart_image_id = 0;
    int percentage_speed = cartpusher_percentage_speed(f);
    f->terrain_usage = TERRAIN_USAGE_ROADS_HIGHWAY;
    building *b = building_get(f->building_id);

    // Assume we're always on the source road network
    // Fixes walkers stopping when deciding to recalculate best destination when on different network
    int road_network_id = b->road_network_id;

    switch (f->action_state) {
        case FIGURE_ACTION_150_ATTACK:
            figure_combat_handle_attack(f);
            break;
        case FIGURE_ACTION_149_CORPSE:
            figure_combat_handle_corpse(f);
            break;
        case FIGURE_ACTION_20_CARTPUSHER_INITIAL:
            set_cart_graphic(f, 1);
            if (!map_routing_citizen_is_passable(f->grid_offset)) {
                f->state = FIGURE_STATE_DEAD;
            }
            if (b->state != BUILDING_STATE_IN_USE || b->figure_id != f->id) {
                f->state = FIGURE_STATE_DEAD;
            }
            if (!road_network_id) {
                f->state = FIGURE_STATE_DEAD;
            }
            f->wait_ticks++;
            if (f->wait_ticks > 30 && road_network_id) {
                determine_cartpusher_destination(f, b, road_network_id);
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_245_CARTPUSHER_WAITING_FOR_DESTINATION:
            set_cart_graphic(f, 1);
            f->wait_ticks++;
            if (f->wait_ticks > NON_STORABLE_RESOURCE_CARTPUSHER_MAX_WAIT_TICKS) {
                f->state = FIGURE_STATE_DEAD;
            } else if ((f->wait_ticks % (NON_STORABLE_RESOURCE_CARTPUSHER_MAX_WAIT_TICKS / 10) == 0)) {
                determine_cartpusher_destination(f, b, road_network_id);
            }
            break;
        case FIGURE_ACTION_21_CARTPUSHER_DELIVERING_TO_WAREHOUSE:
            set_cart_graphic(f, 1);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_24_CARTPUSHER_AT_WAREHOUSE;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                reroute_cartpusher(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->wait_ticks++ > FIGURE_REROUTE_DESTINATION_TICKS) {
                f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                figure_cartpusher_action(f);
                return;
            }
            if (building_get(f->destination_building_id)->state != BUILDING_STATE_IN_USE) {
                f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                f->wait_ticks = 0;
            }
            break;
        case FIGURE_ACTION_22_CARTPUSHER_DELIVERING_TO_GRANARY:
            set_cart_graphic(f, 1);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_25_CARTPUSHER_AT_GRANARY;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                reroute_cartpusher(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                f->wait_ticks = 0;
            } else if (f->wait_ticks++ > FIGURE_REROUTE_DESTINATION_TICKS) {
                f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                figure_cartpusher_action(f);
                return;
            }
            if (building_get(f->destination_building_id)->state != BUILDING_STATE_IN_USE) {
                f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                f->wait_ticks = 0;
            }
            break;
        case FIGURE_ACTION_23_CARTPUSHER_DELIVERING_TO_WORKSHOP:
            set_cart_graphic(f, 1);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_26_CARTPUSHER_AT_WORKSHOP;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                reroute_cartpusher(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;
        case FIGURE_ACTION_246_CARTPUSHER_DELIVERING_TO_MONUMENT:
            if (f->wait_ticks++ >= VALID_MONUMENT_RECHECK_TICKS) {
                if (!building_monument_has_delivery_for_worker(f->id)) {
                    f->state = FIGURE_STATE_DEAD;
                    break;
                }
                f->wait_ticks = 0;
            }
            set_cart_graphic(f, 1);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_247_CARTPUSHER_AT_MONUMENT;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                reroute_cartpusher(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;
        case FIGURE_ACTION_24_CARTPUSHER_AT_WAREHOUSE:
            f->wait_ticks++;
            if (f->wait_ticks > 10) {
                if (building_warehouse_add_resource(building_get(f->destination_building_id), f->resource_id, 1)) {
                    city_health_dispatch_sickness(f);
                    f->action_state = FIGURE_ACTION_27_CARTPUSHER_RETURNING;
                    f->wait_ticks = 0;
                    f->destination_x = f->source_x;
                    f->destination_y = f->source_y;
                } else {
                    figure_route_remove(f);
                    f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                    f->wait_ticks = 0;
                }
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_25_CARTPUSHER_AT_GRANARY:
            f->wait_ticks++;
            if (f->wait_ticks > 5) {
                if (building_granary_add_resource(building_get(f->destination_building_id), f->resource_id, 1)) {
                    city_health_dispatch_sickness(f);
                    f->action_state = FIGURE_ACTION_27_CARTPUSHER_RETURNING;
                    f->wait_ticks = 0;
                    f->destination_x = f->source_x;
                    f->destination_y = f->source_y;
                } else {
                    f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                    determine_cartpusher_destination_food(f, road_network_id);
                }
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_26_CARTPUSHER_AT_WORKSHOP:
            f->wait_ticks++;
            if (f->wait_ticks > 5) {
                building_workshop_add_raw_material(building_get(f->destination_building_id), f->resource_id);
                f->action_state = FIGURE_ACTION_27_CARTPUSHER_RETURNING;
                f->wait_ticks = 0;
                f->destination_x = f->source_x;
                f->destination_y = f->source_y;
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_247_CARTPUSHER_AT_MONUMENT:
            f->wait_ticks++;
            if (f->wait_ticks > 5) {
                if (!building_monument_has_delivery_for_worker(f->id)) {
                    f->state = FIGURE_STATE_DEAD;
                    break;
                }
                building_monument_deliver_resource(building_get(f->destination_building_id), f->resource_id);
                building_monument_remove_delivery(f->id);
                f->action_state = FIGURE_ACTION_27_CARTPUSHER_RETURNING;
                f->wait_ticks = 0;
                f->destination_x = f->source_x;
                f->destination_y = f->source_y;
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_27_CARTPUSHER_RETURNING:
            f->cart_image_id = image_group(GROUP_FIGURE_CARTPUSHER_CART);
            figure_movement_move_ticks_with_percentage(f, 2, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_20_CARTPUSHER_INITIAL;
                f->state = FIGURE_STATE_DEAD;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;
        case FIGURE_ACTION_234_CARTPUSHER_GOING_TO_ROME_CREATED:
            set_cart_graphic(f, 0);
            const map_tile *entry = city_map_entry_point();
            f->action_state = FIGURE_ACTION_235_CARTPUSHER_GOING_TO_ROME;
            f->destination_x = entry->x;
            f->destination_y = entry->y;
            break;
        case FIGURE_ACTION_235_CARTPUSHER_GOING_TO_ROME:
            set_cart_graphic(f, 0);
            f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION || f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            }
    }
    update_image(f);
}

static void determine_granaryman_destination(figure *f, int road_network_id, int remove_resources)
{
    f->is_ghost = 0;
    map_point dst;
    int dst_building_id;
    building *granary = building_get(f->building_id);
    if (!f->resource_id) {
        // getting granaryman
        dst_building_id = building_granary_for_getting(granary, &dst, 400);
        if (!dst_building_id) {
            dst_building_id = building_granary_for_getting(granary, &dst, 0);
        }
        if (dst_building_id) {
            f->loads_sold_or_carrying = 0;
            set_destination(f, FIGURE_ACTION_54_WAREHOUSEMAN_GETTING_FOOD, dst_building_id, dst.x, dst.y);
            if (config_get(CONFIG_GP_CH_GETTING_GRANARIES_GO_OFFROAD)) {
                f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            }
        } else {
            f->state = FIGURE_STATE_DEAD;
            f->is_ghost = 1;
        }
        return;
    }
    // delivering resource
    // priority 1: another granary
    dst_building_id = building_granary_for_storing(f->x, f->y, f->resource_id, road_network_id, 0, 0, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            building_granary_remove_resource(granary, f->resource_id, 100);
        }
        return;
    }
    // priority 2: warehouse
    dst_building_id = building_warehouse_for_storing(0, f->x, f->y, f->resource_id, road_network_id, 0, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            building_granary_remove_resource(granary, f->resource_id, 100);
        }
        return;
    }
    // priority 3: granary even though resource is on stockpile
    dst_building_id = building_granary_for_storing(f->x, f->y, f->resource_id, road_network_id, 1, 0, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            building_granary_remove_resource(granary, f->resource_id, 100);
        }
        return;
    }
    // no one will accept, stand idle
    f->wait_ticks = 2;
}

static void determine_armoury_supplier_destination(figure *f, int road_network_id)
{
    f->is_ghost = 0;

    map_point dst;
    building *armoury = building_get(f->building_id);
    int dst_building_id;

    // Has weapons, deliver to barracks
    if (f->resource_id) { 
        dst_building_id = building_get_barracks_for_weapon(armoury->x, armoury->y, RESOURCE_WEAPONS,
            armoury->road_network_id, &dst);
        if (dst_building_id) {
            set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
            return;
        }
    } else {
        // Go grab weapons
        dst_building_id = building_warehouse_with_resource(armoury->x, armoury->y, RESOURCE_WEAPONS, armoury->road_network_id, 0, &dst, BUILDING_STORAGE_PERMISSION_ARMOURY);
        if (dst_building_id) {
            set_destination(f, FIGURE_ACTION_248_ARMOURY_SUPPLIER_GETTING_WEAPONS, dst_building_id, dst.x, dst.y);
            return;
        }
    }

    // no one will accept, stand idle
    f->wait_ticks = 5;
}

static void remove_resource_from_warehouse(figure *f)
{
    if (f->state != FIGURE_STATE_DEAD) {
        int err = building_warehouse_remove_resource(building_get(f->building_id), f->resource_id, 1);
        if (err) {
            f->state = FIGURE_STATE_DEAD;
        }
    }
}

static void determine_warehouseman_destination(figure *f, int road_network_id, int remove_resources)
{
    f->is_ghost = 0;
    map_point dst;
    int dst_building_id;
    if (!f->resource_id) {
        // getting warehouseman
        dst_building_id = building_warehouse_for_getting(
            building_get(f->building_id), f->collecting_item_id, &dst);
        if (dst_building_id) {
            f->loads_sold_or_carrying = 0;
            set_destination(f, FIGURE_ACTION_57_WAREHOUSEMAN_GETTING_RESOURCE, dst_building_id, dst.x, dst.y);
            f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
        } else {
            f->state = FIGURE_STATE_DEAD;
            f->is_ghost = 1;
        }
        return;
    }
    // delivering resource
    // priority 1: weapons to barracks
    dst_building_id = building_get_barracks_for_weapon(f->x, f->y, f->resource_id, road_network_id, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            remove_resource_from_warehouse(f);
        }
        return;
    }
    // priority 2: raw materials to workshop
    dst_building_id = building_get_workshop_for_raw_material_with_room(f->x, f->y, f->resource_id,
        road_network_id, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            remove_resource_from_warehouse(f);
        }
        return;
    }
    // priority 3: food to granary
    dst_building_id = building_granary_for_storing(f->x, f->y, f->resource_id,
        road_network_id, 0, 0, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            remove_resource_from_warehouse(f);
        }
        return;
    }
    // priority 4: food to getting granary
    dst_building_id = building_getting_granary_for_storing(f->x, f->y, f->resource_id, road_network_id, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            remove_resource_from_warehouse(f);
        }
        return;
    }

    // priority 5: another warehouse to empty this one
    if (building_storage_get(building_get(f->building_id)->storage_id)->empty_all) {
        dst_building_id = building_warehouse_for_storing(f->building_id, f->x, f->y, f->resource_id, -1, 0, &dst);

        // deliver to another warehouse because this one is being emptied
        if (dst_building_id) {
            if (dst_building_id == f->building_id) {
                f->state = FIGURE_STATE_DEAD;
            } else {
                set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
                f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
                if (remove_resources) {
                    remove_resource_from_warehouse(f);
                }
            }
            return;
        }
    }
    // priority 6: raw material to well-stocked workshop
    dst_building_id = building_get_workshop_for_raw_material(f->x, f->y, f->resource_id, road_network_id, &dst);
    if (dst_building_id) {
        set_destination(f, FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE, dst_building_id, dst.x, dst.y);
        if (remove_resources) {
            remove_resource_from_warehouse(f);
        }
        return;
    }
    // no one will accept, stand idle
    f->wait_ticks = 2;
}

static void warehouseman_initial_action(figure *f, int road_network_id, int remove_resources)
{
    if (!road_network_id &&
        (f->terrain_usage == TERRAIN_USAGE_ROADS_HIGHWAY || f->terrain_usage == TERRAIN_USAGE_ROADS)) {
        f->state = FIGURE_STATE_DEAD;
        return;
    }

    f->terrain_usage = TERRAIN_USAGE_ROADS_HIGHWAY;

    building *b = building_get(f->building_id);
    f->is_ghost = 1;
    f->wait_ticks++;
    if (f->wait_ticks > 2) {
        f->wait_ticks = 0;
        if (b->type == BUILDING_GRANARY) {
            determine_granaryman_destination(f, road_network_id, remove_resources);
        } else if (b->type == BUILDING_ARMOURY) {
            determine_armoury_supplier_destination(f, road_network_id);
        } else {
            determine_warehouseman_destination(f, road_network_id, remove_resources);
        }
        set_cart_graphic(f, 1);
    }
    f->image_offset = 0;
}

void figure_warehouseman_action(figure *f)
{
    figure_image_increase_offset(f, 12);
    f->cart_image_id = 0;
    int percentage_speed = cartpusher_percentage_speed(f);
    building *b = building_get(f->building_id);

    if (b->state != BUILDING_STATE_IN_USE || 
        (b->figure_id != f->id && b->figure_id4 != f->id)) {
        f->state = FIGURE_STATE_DEAD;
    }

    // Assume we're always on the source road network
    // Fixes walkers stopping when deciding to recalculate best destination when on different network
    int road_network_id = b->road_network_id;

    switch (f->action_state) {
        case FIGURE_ACTION_150_ATTACK:
            figure_combat_handle_attack(f);
            break;
        case FIGURE_ACTION_149_CORPSE:
            figure_combat_handle_corpse(f);
            break;
        case FIGURE_ACTION_50_WAREHOUSEMAN_CREATED:
        {
            f->terrain_usage = TERRAIN_USAGE_ROADS_HIGHWAY;
            warehouseman_initial_action(f, road_network_id, 1);
            break;
        }
        case FIGURE_ACTION_51_WAREHOUSEMAN_DELIVERING_RESOURCE:
            set_cart_graphic(f, 1);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_52_WAREHOUSEMAN_AT_DELIVERY_BUILDING;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->wait_ticks++ > FIGURE_REROUTE_DESTINATION_TICKS) {
                f->action_state = FIGURE_ACTION_233_WAREHOUSEMAN_RECONSIDER_TARGET;
                figure_warehouseman_action(f);
                return;
            }
            break;
        case FIGURE_ACTION_52_WAREHOUSEMAN_AT_DELIVERY_BUILDING:
            f->wait_ticks++;
            if (f->wait_ticks > 4) {
                b = building_get(f->destination_building_id);
                int delivered = 1;
                switch (b->type) {
                    case BUILDING_GRANARY:
                        delivered = building_granary_add_resource(b, f->resource_id, 0);
                        if (delivered) {
                            city_health_dispatch_sickness(f);
                        }
                        break;
                    case BUILDING_BARRACKS:
                    case BUILDING_GRAND_TEMPLE_MARS:
                        building_barracks_add_weapon(b);
                        break;
                    case BUILDING_WAREHOUSE:
                    case BUILDING_WAREHOUSE_SPACE:
                        delivered = building_warehouse_add_resource(b, f->resource_id, 1);
                        if (delivered) {
                            city_health_dispatch_sickness(f);
                        }
                        break;
                    default: // workshop
                        building_workshop_add_raw_material(b, f->resource_id);
                        break;
                }
                if (delivered) {
                    f->action_state = FIGURE_ACTION_53_WAREHOUSEMAN_RETURNING_EMPTY;
                    f->wait_ticks = 0;
                    f->destination_x = f->source_x;
                    f->destination_y = f->source_y;
                } else {
                    figure_route_remove(f);
                    f->action_state = FIGURE_ACTION_233_WAREHOUSEMAN_RECONSIDER_TARGET;
                    f->wait_ticks = 2;
                }
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_53_WAREHOUSEMAN_RETURNING_EMPTY:
            f->cart_image_id = image_group(GROUP_FIGURE_CARTPUSHER_CART); // empty
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION || f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            }
            break;
        case FIGURE_ACTION_54_WAREHOUSEMAN_GETTING_FOOD:
            if (config_get(CONFIG_GP_CH_GETTING_GRANARIES_GO_OFFROAD)) {
                f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            }
            f->cart_image_id = image_group(GROUP_FIGURE_CARTPUSHER_CART); // empty
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_55_WAREHOUSEMAN_AT_GRANARY;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->wait_ticks++ > FIGURE_REROUTE_DESTINATION_TICKS) {
                f->action_state = FIGURE_ACTION_233_WAREHOUSEMAN_RECONSIDER_TARGET;
                figure_warehouseman_action(f);
                return;
            }
            break;
        case FIGURE_ACTION_55_WAREHOUSEMAN_AT_GRANARY:
            if (config_get(CONFIG_GP_CH_GETTING_GRANARIES_GO_OFFROAD)) {
                f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            }
            f->wait_ticks++;
            if (f->wait_ticks > 4) {
                int resource;
                f->loads_sold_or_carrying = building_granary_remove_for_getting_deliveryman(
                    building_get(f->destination_building_id), building_get(f->building_id), &resource);
                city_health_dispatch_sickness(f);
                f->resource_id = resource;
                f->action_state = FIGURE_ACTION_56_WAREHOUSEMAN_RETURNING_WITH_FOOD;
                f->wait_ticks = 0;
                f->destination_x = f->source_x;
                f->destination_y = f->source_y;
                figure_route_remove(f);
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_56_WAREHOUSEMAN_RETURNING_WITH_FOOD:
            if (config_get(CONFIG_GP_CH_GETTING_GRANARIES_GO_OFFROAD)) {
                f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            }
            // update graphic
            set_cart_graphic(f, 0);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                for (int i = 0; i < f->loads_sold_or_carrying; i++) {
                    building_granary_add_resource(building_get(f->building_id), f->resource_id, 0);
                }
                f->state = FIGURE_STATE_DEAD;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;
        case FIGURE_ACTION_57_WAREHOUSEMAN_GETTING_RESOURCE:
            f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            f->cart_image_id = image_group(GROUP_FIGURE_CARTPUSHER_CART); // empty
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_58_WAREHOUSEMAN_AT_WAREHOUSE;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->wait_ticks++ > FIGURE_REROUTE_DESTINATION_TICKS) {
                f->action_state = FIGURE_ACTION_233_WAREHOUSEMAN_RECONSIDER_TARGET;
                figure_warehouseman_action(f);
                return;
            }
            break;
        case FIGURE_ACTION_58_WAREHOUSEMAN_AT_WAREHOUSE:
            f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            f->wait_ticks++;
            if (f->wait_ticks > 4) {
                f->loads_sold_or_carrying = 0;
                city_health_dispatch_sickness(f);
                while (f->loads_sold_or_carrying < 4 && 0 == building_warehouse_remove_resource(
                    building_get(f->destination_building_id), f->collecting_item_id, 1)) {
                    f->loads_sold_or_carrying++;
                }
                f->resource_id = f->collecting_item_id;
                f->action_state = FIGURE_ACTION_59_WAREHOUSEMAN_RETURNING_WITH_RESOURCE;
                f->wait_ticks = 0;
                f->destination_x = f->source_x;
                f->destination_y = f->source_y;
                figure_route_remove(f);
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_59_WAREHOUSEMAN_RETURNING_WITH_RESOURCE:
            f->terrain_usage = TERRAIN_USAGE_PREFER_ROADS_HIGHWAY;
            set_cart_graphic(f, 0);
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                for (int i = 0; i < f->loads_sold_or_carrying; i++) {
                    building_warehouse_add_resource(building_get(f->building_id), f->resource_id, 1);
                }
                f->state = FIGURE_STATE_DEAD;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;
        case FIGURE_ACTION_248_ARMOURY_SUPPLIER_GETTING_WEAPONS:
            f->cart_image_id = image_group(GROUP_FIGURE_CARTPUSHER_CART); // empty
            figure_movement_move_ticks_with_percentage(f, 1, percentage_speed);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                f->action_state = FIGURE_ACTION_249_ARMOURY_SUPPLIER_AT_WAREHOUSE;
                f->wait_ticks = 0;
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            } else if (f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            } else if (f->wait_ticks++ > FIGURE_REROUTE_DESTINATION_TICKS) {
                f->action_state = FIGURE_ACTION_233_WAREHOUSEMAN_RECONSIDER_TARGET;
                figure_warehouseman_action(f);
                return;
            }
            break;
        case FIGURE_ACTION_249_ARMOURY_SUPPLIER_AT_WAREHOUSE:
            f->wait_ticks++;
            if (f->wait_ticks > 4) {
                f->loads_sold_or_carrying = 0;
                city_health_dispatch_sickness(f);
                if (0 == building_warehouse_remove_resource(building_get(f->destination_building_id), 
                    f->collecting_item_id, 1)) {
                    f->loads_sold_or_carrying++;
                    f->resource_id = f->collecting_item_id;
                    f->destination_building_id = 0;
                    figure_route_remove(f);
                }
                warehouseman_initial_action(f, road_network_id, 0);
            }
            f->image_offset = 0;
            break;
        case FIGURE_ACTION_233_WAREHOUSEMAN_RECONSIDER_TARGET:
            warehouseman_initial_action(f, road_network_id, 0);
            break;
    }
    update_image(f);
}
