credit to Shift2Ace/sba-2026 for the original base commitment and ideas.
modified and integraded due to its complexity for most candidates to vibe coding or changes.

no DC motor defined but a testing servo motor code exist (at gpio 32)
(at more motors at other not in use gpios and please correspond to storage id)
relay for dispensing water signal pin at gpio33
rtc and i2c follow the orignal (a must component for ip address and rtc/ntp)
buzzer at gpio25
(add more component at other gpio if needed...)

upload the code at arduino with espressif library installed (ESP32 Devkit)
