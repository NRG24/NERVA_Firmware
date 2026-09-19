/* Syntax-check stub. */
#ifndef ZSTUB_GPIO_H_
#define ZSTUB_GPIO_H_
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
typedef uint32_t gpio_port_pins_t;
typedef uint8_t gpio_pin_t;
typedef uint32_t gpio_flags_t;
#define GPIO_INPUT		(1U << 16)
#define GPIO_OUTPUT		(1U << 17)
#define GPIO_PULL_UP		(1U << 4)
#define GPIO_PULL_DOWN		(1U << 5)
#define GPIO_OUTPUT_ACTIVE	(1U << 18)
#define GPIO_OUTPUT_INACTIVE	(1U << 19)
#define GPIO_INT_DISABLE	(1U << 21)
#define GPIO_INT_EDGE_RISING	(1U << 22)
#define GPIO_INT_EDGE_FALLING	(1U << 23)
struct gpio_callback;
typedef void (*gpio_callback_handler_t)(const struct device *port,
					struct gpio_callback *cb,
					gpio_port_pins_t pins);
struct gpio_callback { gpio_callback_handler_t handler; gpio_port_pins_t pin_mask; void *node; };
int gpio_pin_configure(const struct device *port, gpio_pin_t pin, gpio_flags_t flags);
int gpio_pin_get_raw(const struct device *port, gpio_pin_t pin);
int gpio_pin_set_raw(const struct device *port, gpio_pin_t pin, int value);
int gpio_pin_interrupt_configure(const struct device *port, gpio_pin_t pin, gpio_flags_t flags);
void gpio_init_callback(struct gpio_callback *cb, gpio_callback_handler_t handler,
			gpio_port_pins_t pin_mask);
int gpio_add_callback(const struct device *port, struct gpio_callback *cb);
int gpio_remove_callback(const struct device *port, struct gpio_callback *cb);
#endif
