#ifndef WEATHER_WEATHER_CLIENT_H_
#define WEATHER_WEATHER_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_CITY_TEXT_SIZE        (32U)
#define WEATHER_CONDITION_TEXT_SIZE   (32U)
#define WEATHER_TEMPERATURE_TEXT_SIZE (12U)
#define WEATHER_WIND_TEXT_SIZE        (32U)
#define WEATHER_FORECAST_DAY_COUNT    (3U)

typedef struct st_weather_forecast_day
{
    char condition[WEATHER_CONDITION_TEXT_SIZE];
    char low_temperature[WEATHER_TEMPERATURE_TEXT_SIZE];
    char high_temperature[WEATHER_TEMPERATURE_TEXT_SIZE];
} weather_forecast_day_t;

typedef struct st_weather_data
{
    char city[WEATHER_CITY_TEXT_SIZE];
    char current_temperature[WEATHER_TEMPERATURE_TEXT_SIZE];
    char wind_direction[WEATHER_WIND_TEXT_SIZE];
    char wind_force[WEATHER_WIND_TEXT_SIZE];
    weather_forecast_day_t forecast[WEATHER_FORECAST_DAY_COUNT];
} weather_data_t;

bool weather_client_init(void);
void weather_client_poll(uint32_t now_ms);
bool weather_client_request(uint32_t now_ms);
bool weather_client_is_busy(void);
bool weather_client_take_data(weather_data_t * data);
char const * weather_client_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_WEATHER_CLIENT_H_ */
