# Adaptive Cruise Control

An extension of the autonomous vehicle stack autoware to include adaptive cruise control and platooning capabilites

## Motivation

In response to the evolving landscape of automotive engineering, the integration of the Adaptive Cruise Control (ACC) system offers a solution that prioritizes the following
- Safety Enhancement, Convenience, Traffic Flow Optimization, Fuel Efficiency

## Project Objective

Can we use autonomous vehicle software to have a vehicle drive itself on highways and adapt to various dynamic situations?
- Sensor Utilization: Utilize LiDAR and Depth Camera sensors for informed decision-making
- Automation: Automate driving scenarios to enhance algorithm and AI performance on complex road networks.
- AW Framework Enhancement: Integrate functionality for Latitude (LAT), Longitude (LONG), and LATLONG platooning.
- Safety Assurance: Ensure thorough testing of all vehicle software to guarantee safety compliance

## Solution Architecture

![image](https://github.com/Mohammad0336/Adaptive_Cruise_Control/assets/81828400/99960b22-6457-4b6e-8f75-66fd869d6a1a)

## Platooning Algorithm


The ACC system incorporates automobile platooning which allows for vehicles to travel in synchronized formations:
- Platooning incorporates grouping in the Lateral, Longitudinal and Lat-Long planes [X, Y, Z]
- Creates fleet of vehicles following each other from a set distance calculated with the LiDAR sensors
- Advanced searching for vehicles and obstacle avoidance
- Able to locate and adapt to situations involving vehicles in adjacent lanes

![image](https://github.com/Mohammad0336/Adaptive_Cruise_Control/assets/81828400/1fe0249d-2620-4703-a699-c56a08d1b4c3)


#### autoware.universe

For Autoware's general documentation, see [Autoware Documentation](https://autowarefoundation.github.io/autoware-documentation/).

For detailed documents of Autoware Universe components, see [Autoware Universe Documentation](https://autowarefoundation.github.io/autoware.universe/).

---
