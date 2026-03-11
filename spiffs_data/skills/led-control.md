# LED Control

Control the onboard LED on the ESP32-S3 board.
1
## When to use
When the user asks to turn on, turn off, blink, or control the LED light.

## How to use
1. Use the `led_control` tool to control the LED
2. The tool accepts commands: "on", "off", "blink", or "pulse"
3. For blink and pulse, you can optionally specify duration in milliseconds
4. Return a confirmation message to the user

## Example
User: "Turn on the LED"
→ led_control {"command": "on"}
→ "LED turned on"

User: "Make the LED blink"
→ led_control {"command": "blink", "duration": 500}
→ "LED is now blinking"

User: "Turn off the LED"
→ led_control {"command": "off"}
→ "LED turned off"