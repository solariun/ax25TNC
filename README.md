AX25 lib + TNC

Here is the complete and extensive development manual. It is structured as a comprehensive guide for developers to understand both the theoretical mechanics of the protocols and the practical implementation using the C++ classes we developed.

You can save this as `Packet_Radio_Developer_Manual.md`.

---

```markdown
# The Packet Radio Developer's Handbook: C++ Implementation Guide for KISS, AX.25, and APRS

This manual provides a deep dive into the architecture, theory, and C++ implementation of the Amateur Packet Radio stack. It is designed for developers building TNC interfaces, AX.25 state machines, and APRS applications from scratch using strictly POSIX-compliant C++11 without external libraries or "magic numbers."

---

## 1. Architectural Overview

The stack is built on a strict layering mechanism, closely mapping to the OSI model.


1.  **Hardware/Serial Layer (KISS):** Manages the physical byte stream over a serial port (or Bluetooth serial profile). It ensures frame boundaries are respected.
2.  **Data Link Layer (AX.25):** Handles addressing, digipeater routing, payload encapsulation, and connection state (sliding window, ACKs, and sequence numbers).
3.  **Application Layer (APRS/Chat):** The formatted payload that resides inside the AX.25 Information (I) or Unnumbered Information (UI) frames.

### 1.1 The "Zero Magic Numbers" Principle
All byte values, masks, shifts, and protocol flags are centralized in `config::` and `ax25::` namespaces. This prevents hardcoded hex values in the logic, ensuring the code is maintainable and self-documenting.

---

## 2. The KISS Protocol Layer (`KissTNC` Class)

The KISS (Keep It Simple, Stupid) protocol encapsulates AX.25 frames for transmission over a serial line to the TNC (Terminal Node Controller).

### 2.1 How KISS Works (Frame Delimiting & Byte Stuffing)
Serial ports transmit continuous streams of bytes. The TNC needs to know where a packet begins and ends. KISS uses the `FEND` (`0xC0`) byte as a boundary. 

If the AX.25 payload natively contains a `0xC0` byte, it would prematurely end the frame. To prevent this, KISS uses **Byte Stuffing** (Escaping).

* If payload contains `0xC0` (`FEND`), send `0xDB 0xDC` (`FESC` + `TFEND`).
* If payload contains `0xDB` (`FESC`), send `0xDB 0xDD` (`FESC` + `TFESC`).

### 2.2 `KissTNC` Implementation
The `KissTNC` class manages the POSIX serial connection and the escaping/unescaping logic.

**Implementation Example:**
```cpp
KissTNC tnc;
if (!tnc.connect("/tmp/ttyKISS", 9600)) {
    throw std::runtime_error("Serial connection failed.");
}

// Writing to the TNC (Escaping is handled internally)
ax25::Frame my_frame = /* ... */;
tnc.write_frame(my_frame);

// Reading from the TNC (Unescaping is handled internally)
// This function blocks until a full KISS frame (bounded by 0xC0) is received.
std::vector<uint8_t> raw_ax25 = tnc.read_ax25_payload();

```

---

## 3. The AX.25 Data Link Layer (`Address` and `Frame` Classes)

AX.25 is the core routing protocol. Every frame requires a Destination, a Source, and an optional path of Digipeaters.

### 3.1 The `Address` Class

AX.25 addresses are unique. A callsign is up to 6 characters, padded with spaces, and includes a 1-byte SSID (Secondary Station Identifier).

To prevent address bytes from triggering control flags, AX.25 **shifts all ASCII characters 1 bit to the left**. The SSID byte also contains flags indicating Command/Response (`C/R`) and whether the address is the last in the header (`Extension Bit`).

**Implementation Example:**

```cpp
// Create an address using the helper
ax25::Address source = ax25::Address::from_string("N1CALL-0");
ax25::Address dest = ax25::Address::from_string("G2UGK-1");
ax25::Address digi = ax25::Address::from_string("WIDE1-1");

```

### 3.2 The `Frame` Class

The `Frame` class constructs the byte array to be passed to the KISS layer. It manages the Address fields, the Control byte, the Protocol Identifier (PID), and the Payload.

**Implementation Example:**

```cpp
ax25::Frame frame(dest, source);
frame.add_digipeater(digi);
frame.set_control(ax25::U_FRAME_UI); // Unnumbered Information
frame.set_pid(ax25::PID_NONE);       // No layer 3 protocol
frame.set_payload({'H', 'e', 'l', 'l', 'o'});

std::vector<uint8_t> encoded_bytes = frame.encode();

```

---

## 4. Connected Mode & The Receive Loop

Connected mode establishes a reliable, error-corrected session between two nodes. This requires managing sequence numbers and acknowledging received data.

### 4.1 Connection Setup and Teardown

* **Connect:** Send a `SABM` (Set Asynchronous Balanced Mode) frame. The target responds with a `UA` (Unnumbered Acknowledge) frame.
* **Disconnect:** Send a `DISC` frame. The target responds with `UA`.

### 4.2 Sequence Numbers: `N(S)` and `N(R)`

AX.25 uses a sliding window protocol to ensure packets are received in order and none are dropped.

* **`N(S)` (Send Sequence):** The ID of the packet you are currently sending (0-7).
* **`N(R)` (Receive Sequence):** The ID of the *next* packet you expect to receive (0-7).

The Control Byte for an Information (I) frame is constructed by shifting these numbers:
`Control Byte = (N(R) << 5) | (N(S) << 1) | 0x00`

### 4.3 The Receive Loop & Auto-ACK State Machine

When a BBS sends you a burst of text, you must reply with an `RR` (Receive Ready) frame. If you don't, the BBS assumes the packets were lost, halts transmission, and retransmits them.

The background RX thread serves two critical purposes:

1. **Strict Sequence Checking:** It verifies that the incoming `N(S)` matches our expected `N(R)`. If a packet arrives out of order (e.g., missed packet 2, received packet 3), it ignores the payload.
2. **Auto-Acknowledgment:** Upon receiving a valid I-Frame, it instantly transmits an `RR` frame with the updated `N(R)`, unlocking the BBS's transmission buffer to send the next lines.

**Implementation Example (The RX Loop Logic):**

```cpp
uint8_t ctrl = frame.get_control();

if ((ctrl & ax25::CONTROL_I_FRAME_MASK) == ax25::CONTROL_I_FRAME_BIT) { 
    // It's an Information (I) Frame containing text
    uint8_t received_ns = (ctrl >> config::AX25_NS_SHIFT) & config::AX25_SEQ_MASK;
    uint8_t received_nr = (ctrl >> config::AX25_NR_SHIFT) & config::AX25_SEQ_MASK;
    
    if (received_ns == next_nr.load()) {
        // Packet is perfectly in order!
        // 1. Advance our expected Receive number
        next_nr = (received_ns + 1) % config::AX25_SEQ_MODULO; 
        // 2. Sync our Send number with their Receive acknowledgment
        next_ns = received_nr; 
        
        // 3. Print the payload
        print_payload(frame.get_payload());
    }
    
    // 4. Always send an ACK (Receive Ready) to tell the BBS what we expect next
    uint8_t rr_control = (next_nr.load() << config::AX25_NR_SHIFT) | config::AX25_RR_BASE;
    send_ack_frame(tnc, target, me, rr_control);
}

```

---

## 5. Connectionless Mode: UI Messages

UI (Unnumbered Information) frames do not use sequence numbers, `SABM`, or ACKs. They are fire-and-forget. If two radios transmit at once and the packet collides, it is permanently lost. This is used for general beacons and CQ calls.

**Implementation Example:**

```cpp
ax25::Frame ui_frame(target, me);
ui_frame.set_control(ax25::U_FRAME_UI); // 0x03
ui_frame.set_pid(ax25::PID_NONE);       // 0xF0
ui_frame.set_payload(string_to_vector("General CQ from N1CALL"));
tnc.write_frame(ui_frame);

```

---

## 6. APRS (Automatic Packet Reporting System)

APRS operates entirely on Layer 2 UI Frames. Instead of point-to-point connections, nodes broadcast specific payloads to a generic destination (like `APRS`), and digipeaters repeat them to a wider audience or to Internet Gateways (iGates).

### 6.1 APRS Payloads

The APRS payload type is defined by the very first ASCII character in the payload string.

#### Location Payload (Starts with `!`)

Used to broadcast GPS coordinates.

```cpp
std::string payload = "!4903.50N/07201.75W-Walking the dog";
// '!' = Location without timestamp
// 'N/W' = Latitude/Longitude formatting
// '-' = APRS Symbol Table identifier (House/QTH)

```

#### Status Payload (Starts with `>`)

Used to broadcast node status or BBS availability.

```cpp
std::string payload = ">N1CALL Node is active on 144.390";

```

#### Text Message Payload (Starts with `:`)

Used for direct radio-to-radio chat.
*Rule:* The target callsign **must be exactly 9 characters long**, padded with spaces, followed by a colon.

```cpp
// Sending a message to N2CALL
std::string target_padded = "N2CALL   "; 
std::string msg_id = "{01"; // Application-level ACK request
std::string payload = ":" + target_padded + ":Hello, are you there?" + msg_id;

```

### 6.2 APRS Frame Assembly Example

```cpp
ax25::Address aprs_dest("APRS", 0);
ax25::Address me("N1CALL", 9);

ax25::Frame aprs_frame(aprs_dest, me);
// WIDE1-1 is the standard paradigm telling local digipeaters to repeat this once
aprs_frame.add_digipeater(ax25::Address("WIDE1", 1)); 

aprs_frame.set_control(ax25::U_FRAME_UI);
aprs_frame.set_pid(ax25::PID_NONE);

std::string aprs_payload = "!4903.50N/07201.75W-Testing C++ Implementation";
aprs_frame.set_payload(std::vector<uint8_t>(aprs_payload.begin(), aprs_payload.end()));

tnc.write_frame(aprs_frame);

```

---

**End of Manual**

```

```
