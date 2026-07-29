# RWA RTK Rover

## Key Aspects of the Task Scheduling

### FreeRTOS Configuration

- 4 concurrent tasks running on a dual-core ESP32
- Core 0: RTK correction data and position tasks (WiFi/GNSS heavy)
- Core 1: BLE communication tasks (IMU and position transmission)
- All tasks have Priority 2 (equal priority with time-slicing)

### Task Timing

- `task_rtk_get_corrrection_data`: 1000ms intervals
- `task_rtk_get_rover_position`: 100ms intervals
- `task_bno_orientation_via_ble`: 12ms intervals (fastest - for head tracking)
- `task_send_rtk_position_via_ble`: 100ms intervals

### Inter-Task Communication

- Mutex Semaphore: Protects shared GNSS resources
- Two Queues:
  - `xQueueCoord`: Position coordinates
  - `xQueueAccuracy`: Position accuracy data
- Global Flag: `beginPositioning` - synchronizes position reading with correction data availability

### Task Dependencies

- Task 1 must establish RTCM correction stream before Task 2 begins positioning
- Tasks 3 & 4 wait for BLE connection before operation
- Task 4 consumes data produced by Task 2 via queues
- This architecture ensures real-time head tracking (~12ms) while maintaining accurate RTK positioning and reliable wireless communication.

## Flowchart

```mermaid

flowchart TD
    A[System Start] --> B[Hardware Setup]
    B --> C[Initialize LittleFS]
    C --> D[Setup WiFi Connection]
    D --> E[Setup BLE]
    E --> F[Create FreeRTOS Resources]

    F --> G[Create Mutex Semaphore]
    G --> H[Create Queues:<br/>xQueueCoord & xQueueAccuracy]

    H --> I[Create FreeRTOS Tasks]
    I --> T1[Task 1:<br/>task_rtk_get_corrrection_data<br/>Core 0, Priority 2]
    I --> T2[Task 2:<br/>task_rtk_get_rover_position<br/>Core 0, Priority 2]
    I --> T3[Task 3:<br/>task_bno_orientation_via_ble<br/>Core 1, Priority 2]
    I --> T4[Task 4:<br/>task_send_rtk_position_via_ble<br/>Core 1, Priority 2]

    I --> L[Main Loop:<br/>Continuous Test Execution]

    %% Task 1 Details
    T1 --> T1A[Setup GNSS Module]
    T1A --> T1B[Connect to NTRIP Caster]
    T1B --> T1C[Receive RTCM Correction Data]
    T1C --> T1D[Push RTCM to GNSS via I2C<br/>Using Mutex]
    T1D --> T1E[Send GGA Position to Caster]
    T1E --> T1F[Delay 1000ms]
    T1F --> T1C

    %% Task 2 Details
    T2 --> T2A[Wait for beginPositioning Flag]
    T2A --> T2B[Get Position Data<br/>Using Mutex]
    T2B --> T2C[Send Coordinates to xQueueCoord]
    T2C --> T2D[Send Accuracy to xQueueAccuracy<br/>if changed]
    T2D --> T2E[Delay 100ms]
    T2E --> T2B

    %% Task 3 Details
    T3 --> T3A[Wait for BLE Connection]
    T3A --> T3B[Setup BNO080 IMU]
    T3B --> T3C[Read Quaternion Data]
    T3C --> T3D[Convert to Euler Angles<br/>Yaw, Pitch, Linear Acceleration]
    T3D --> T3E[Send via BLE Characteristic]
    T3E --> T3F[Delay 12ms]
    T3F --> T3C

    %% Task 4 Details
    T4 --> T4A[Wait for BLE Connection]
    T4A --> T4B[Receive from xQueueCoord]
    T4B --> T4C[Receive from xQueueAccuracy]
    T4C --> T4D[Format Position Data]
    T4D --> T4E[Send via BLE Characteristics]
    T4E --> T4F[Delay 100ms]
    T4F --> T4B

    %% Data Flow
    T1D -.-> T2A
    T2C -.-> T4B
    T2D -.-> T4C

    %% Styling
    classDef taskClass fill:#1a365d,stroke:#63b3ed,stroke-width:2px,color:#ffffff
    classDef setupClass fill:#44337a,stroke:#b794f6,stroke-width:2px,color:#ffffff
    classDef loopClass fill:#276749,stroke:#68d391,stroke-width:2px,color:#ffffff
    classDef dataClass fill:#744210,stroke:#f6ad55,stroke-width:2px,color:#ffffff

    class T1,T2,T3,T4 taskClass
    class A,B,C,D,E,F,G,H,I setupClass
    class L loopClass
    class T1C,T1D,T2C,T2D,T4B,T4C dataClass
```
