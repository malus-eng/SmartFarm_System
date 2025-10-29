# Overview
This project simulates a smart monitoring system developed specifically for farms. The system consists of three main components: an Arduino MKR WiFi 1010, a force sensor (FSR406), and an infrared break-beam sensor. The device detects the weight of incoming animals to determine whether they are humans or pests such as rabbits, foxes, and voles, which could harm poultry and crops. Furthermore, when an intruder is detected, the system uses the infrared break-beam sensor to determine the number of intruders and the severity of the intrusion. Finally, a reset button allows farmers to acknowledge the alarm and restart the detection cycle.

## Concept & Inspiration
The inspiration for this project mainly came from my understanding of the current state of livestock farming in the UK. I learned from a report that European rabbits have a significant impact on British livestock farming, as they frequently sneak into pastures and graze on grass and tree roots, causing grassland degradation. Faced with this problem, I noticed that rabbits and humans could be quickly distinguished by recognizing the weight difference. Therefore, I began to sketch out my ideas on paper, as shown in the first draft of the system below:
![Smart Farm Sketch](FARM_SKETCH.jpg)
## System Design
I've included a circuit diagram and detailed explanation of the structure of my entire system.
The entire system consists of five main electronic components: an FSR406 pressure sensor, an IR break beam, a push button, an Arduino MKR WiFi 1010, and a Vespera LED Sphere.
- FSR406 Pressure Sensor detects the weight of an object entering the system via pressure.
- IR Break Beam records the number of objects entering the system and detects if any object is covering the pressure sensor.
- Push Button confirms alarms and resets the entire system.
- Arduino MKR WiFi 1010 handles logic and MQTT communication.
- Vespera LED Sphere provides alarm and status visualization via light color.

## Hardware Setup
Arduino MKR WiFi 1010：purpose-Main microcontroller, Power-3.3V
FSR Sensor ：model-FSR406 + 10kΩ,purpose-Detect weight, Pin-A0
IR Break Beam：model-3mm pair, purpose-Detect crossing and covering, Pin-
Button：model-L03,purpose-Reset system, Pin-D2, Power-3.3V
> All grounds are connected together (GND shared across FSR, IR, and button).

## Software Logic
Next, I will briefly explain the core software logic involved in this project to help you understand the core logic of the system.
(1) FSR Reading and Weight Classification
```
int fsrValue = analogRead(A0);
if (fsrValue > HUMAN_THRESHOLD) {
  publishColor(0, 255, 0);
  delay(3000);
  publishColor(0, 0, 255);
} else {
  publishColor(255, 0, 0); 
  intrusionLocked = true;   
}
```
When the FSR is triggered, it will output an analog voltage as a signal to the Arduino. When the value of this signal exceeds the set threshold, it will automatically identify the passing object as a human; otherwise, it will enter alarm mode and light up a red LED.
(2) IR Break Beam Intrusion Counter
```
if (beamInterrupted && intrusionLocked) {
  intrusionCount++;
  displayNumber(intrusionCount);
}
```
Once the device lights up red and enters a standby state, the IR Break Beam will activate its counting function. Whenever an object passes over the pressure sensor, it will increase the system's internal count of invading organisms.

(3) Reset Button
```
if (digitalRead(resetPin) == LOW) {
  intrusionLocked = false;
  intrusionCount = 0;
  publishColor(0, 0, 255);
}
```
The button is mainly used for users to confirm receipt of information and reset data. Once the button is pressed, the counter will return to zero and the entire system will return to the default state.

(4)Beam Misalignment Watchdog
```
if (beamBlockedDuration > 7000) {
  publishColor(255, 0, 255);  
}
```
If the IR beam is continuously blocked for more than seven seconds, the system will automatically recognize that there is debris obstructing the pressure sensor and it needs to be removed. At the same time, the Arduino will also transmit a signal to keep the LED flashing.

## Testing & Iterations
1. Function: Simply includes a pressure sensor to detect the weight of an object and determine whether it is a person or an animal. Reflection: After determining the pressure comes from a harmful organism, the LED should remain red until the user confirms receiving the alarm; a confirmation device needs to be added.
2. Function: After adding a button, if the system enters alarm mode, the user needs to press the button to confirm receiving the alarm. This design ensures the user receives the system's alarm. Reflection: If an object such as a rock is covering the pressure sensor, it may cause the system to misjudge; the sensor needs to detect whether there is an object above it.
3. Function: With the addition of an IR Break Beam, it means it can detect whether there is an object above the pressure sensor. If the laser is blocked for a long time, an alarm will be sounded. Reflection: Since a laser sensor has been added, can it be further used for counting?
4. Function: With the addition of a new code to the laser, the number of objects that the laser sensor can detect is determined, enabling a counting function. Reflection: How to distinguish between underweight children and animals?

## Future Improvements
1. Consider how to identify and distinguish between young children and pests (potentially by increasing the number of laser sensors to differentiate them by measuring height and movement speed).

2. Current alarm devices may sometimes fail to attract the user's attention and prompt quick action. Perhaps the system could be connected to the farm gate or power grid, or emit an audible alarm to allow the user and the system to respond to intruders as quickly as possible.
