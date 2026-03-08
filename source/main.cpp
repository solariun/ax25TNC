#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <thread>
#include <atomic>
#include <mutex>

// ========================================================================
// 1. CONFIGURAÇÕES E CONSTANTES GERAIS (ZERO MAGIC NUMBERS)
// ========================================================================
namespace config {
    // Protocolo KISS
    constexpr uint8_t KISS_FEND       = 0xC0;
    constexpr uint8_t KISS_FESC       = 0xDB;
    constexpr uint8_t KISS_TFEND      = 0xDC;
    constexpr uint8_t KISS_TFESC      = 0xDD;
    constexpr uint8_t KISS_CMD_DATA   = 0x00;
    
    // Controles Seriais e Sistema
    constexpr int     SERIAL_ERROR    = -1;
    constexpr int     POLL_TIMEOUT_MS = 5000;
    constexpr useconds_t POLL_DELAY_US= 1000; // 1 ms

    // AX.25 Control Field para Conexão
    constexpr uint8_t AX25_SABM_CMD   = 0x2F | 0x10; // SABM + Poll Bit
    constexpr uint8_t AX25_UA_RSP     = 0x63 | 0x10; // UA + Final Bit
    constexpr uint8_t AX25_DISC_CMD   = 0x43 | 0x10; // DISC + Poll Bit
    constexpr uint8_t AX25_RR_BASE    = 0x01;        // Receive Ready (S-Frame base)
    
    // AX.25 Shifts, Máscaras e Modulos
    constexpr uint8_t AX25_NS_SHIFT   = 1;
    constexpr uint8_t AX25_NR_SHIFT   = 5;
    constexpr uint8_t AX25_SEQ_MODULO = 8;
    constexpr uint8_t AX25_SEQ_MASK   = 0x07; // Extrai os 3 bits de sequencia (0-7)
    constexpr uint8_t AX25_PF_BIT     = 0x10; // Poll/Final Bit
    
    // Interface de Usuário e Terminadores
    constexpr char    CHAR_CR         = '\r'; // Terminador de comando padrão para BBS/BPQ
    const std::string CMD_DISCONNECT  = "...d";
}

namespace ax25 {
    // Dimensionamento
    constexpr size_t  CALLSIGN_MAX_LENGTH       = 6;
    constexpr size_t  MAC_ADDRESS_LENGTH        = 7;
    
    // SSID e Shifts
    constexpr uint8_t MIN_SSID                  = 0;
    constexpr uint8_t MAX_SSID                  = 15;
    constexpr uint8_t CALLSIGN_SHIFT            = 1;
    constexpr uint8_t SSID_SHIFT                = 1;
    
    // Máscaras SSID
    constexpr uint8_t EXTENSION_BIT_MASK        = 0x01;
    constexpr uint8_t SSID_MASK                 = 0x1E;
    constexpr uint8_t SSID_RESERVED_BITS_MASK   = 0x60;
    constexpr uint8_t COMMAND_RESPONSE_BIT      = 0x80;
    constexpr uint8_t HAS_BEEN_REPEATED_BIT     = 0x80;
    
    // Control Field bits e Tipos de Frame
    constexpr uint8_t CONTROL_I_FRAME_MASK      = 0x01;
    constexpr uint8_t CONTROL_I_FRAME_BIT       = 0x00;
    constexpr uint8_t CONTROL_S_FRAME_MASK      = 0x03;
    constexpr uint8_t CONTROL_S_FRAME_BIT       = 0x01;
    constexpr uint8_t U_FRAME_UI                = 0x03;
    
    // Protocol Identifiers (PID)
    constexpr uint8_t PID_NONE                  = 0xF0;
    constexpr char    CALLSIGN_PAD_CHAR         = ' ';

    // ========================================================================
    // 2. CLASSES AX.25 (Address e Frame)
    // ========================================================================
    class Address {
    private:
        std::string callsign;
        uint8_t ssid;
        bool command_response;
        bool has_been_repeated;
        bool extension;

    public:
        Address(const std::string& call, uint8_t ss = MIN_SSID) 
            : callsign(call), ssid(ss), command_response(false), 
              has_been_repeated(false), extension(false) {
            if (callsign.length() > CALLSIGN_MAX_LENGTH) throw std::invalid_argument("Callsign too long.");
            if (ssid > MAX_SSID) throw std::invalid_argument("SSID out of range.");
        }

        bool is_extension() const { return extension; }

        std::vector<uint8_t> encode(bool is_last) const {
            std::vector<uint8_t> buffer(MAC_ADDRESS_LENGTH, CALLSIGN_PAD_CHAR << CALLSIGN_SHIFT);
            for (size_t i = 0; i < callsign.length(); ++i) {
                buffer[i] = callsign[i] << CALLSIGN_SHIFT;
            }
            uint8_t ssid_byte = (ssid << SSID_SHIFT) & SSID_MASK;
            ssid_byte |= SSID_RESERVED_BITS_MASK;
            if (command_response || has_been_repeated) ssid_byte |= COMMAND_RESPONSE_BIT;
            if (is_last) ssid_byte |= EXTENSION_BIT_MASK;
            
            buffer[CALLSIGN_MAX_LENGTH] = ssid_byte;
            return buffer;
        }

        static Address from_string(const std::string& full_call) {
            std::string call = full_call;
            uint8_t ssid = 0;
            size_t dash = full_call.find('-');
            if (dash != std::string::npos) {
                call = full_call.substr(0, dash);
                ssid = static_cast<uint8_t>(std::stoi(full_call.substr(dash + 1)));
            }
            return Address(call, ssid);
        }
    };

    class Frame {
    private:
        Address destination;
        Address source;
        std::vector<Address> digipeaters;
        uint8_t control;
        uint8_t pid;
        std::vector<uint8_t> payload;

    public:
        Frame(const Address& dest, const Address& src) 
            : destination(dest), source(src), control(U_FRAME_UI), pid(PID_NONE) {}

        void add_digipeater(const Address& digi) { digipeaters.push_back(digi); }
        void set_payload(const std::vector<uint8_t>& data) { payload = data; }
        std::vector<uint8_t> get_payload() const { return payload; }
        uint8_t get_control() const { return control; }
        void set_pid(uint8_t p) { pid = p; }
        void set_control(uint8_t c) { control = c; }

        std::vector<uint8_t> encode() const {
            std::vector<uint8_t> frame_data;
            auto dest_encoded = destination.encode(false);
            frame_data.insert(frame_data.end(), dest_encoded.begin(), dest_encoded.end());

            bool no_digis = digipeaters.empty();
            auto src_encoded = source.encode(no_digis);
            frame_data.insert(frame_data.end(), src_encoded.begin(), src_encoded.end());

            for (size_t i = 0; i < digipeaters.size(); ++i) {
                bool is_last = (i == digipeaters.size() - 1);
                auto digi_encoded = digipeaters[i].encode(is_last);
                frame_data.insert(frame_data.end(), digi_encoded.begin(), digi_encoded.end());
            }

            frame_data.push_back(control);
            if (control == U_FRAME_UI || (control & CONTROL_I_FRAME_MASK) == CONTROL_I_FRAME_BIT) {
                frame_data.push_back(pid);
            }
            if (!payload.empty()) {
                frame_data.insert(frame_data.end(), payload.begin(), payload.end());
            }
            return frame_data;
        }

        static Frame decode_payload_only(const std::vector<uint8_t>& data) {
            Frame f(Address("DUMMY"), Address("DUMMY"));
            if (data.size() < MAC_ADDRESS_LENGTH * 2) return f;

            size_t offset = MAC_ADDRESS_LENGTH * 2;
            // Pula digipeaters verificando o Extension Bit do endereco anterior
            while (offset < data.size() && (data[offset-1] & EXTENSION_BIT_MASK) == 0) {
                offset += MAC_ADDRESS_LENGTH; 
            }
            
            if (offset >= data.size()) return f;
            uint8_t ctrl = data[offset++];
            f.set_control(ctrl); // Salva o controle para a maquina de estados RX
            
            if (ctrl == U_FRAME_UI || (ctrl & CONTROL_I_FRAME_MASK) == CONTROL_I_FRAME_BIT) {
                if(offset < data.size()) offset++; // Pula o PID
            }
            
            if (offset < data.size()) {
                f.set_payload(std::vector<uint8_t>(data.begin() + offset, data.end()));
            }
            return f;
        }
    };
}

// ========================================================================
// 3. CAMADA DE ENLACE (KISS TNC SERIAL)
// ========================================================================
class KissTNC {
private:
    int fd;

    speed_t map_baud_rate(int baud) {
        switch (baud) {
            case 1200:  return B1200;
            case 9600:  return B9600;
            case 19200: return B19200;
            case 38400: return B38400;
            case 57600: return B57600;
            default:    return B9600;
        }
    }

public:
    KissTNC() : fd(config::SERIAL_ERROR) {}
    ~KissTNC() { if (fd != config::SERIAL_ERROR) close(fd); }

    bool connect(const std::string& device, int baud_rate) {
        fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
        if (fd == config::SERIAL_ERROR) return false;

        struct termios tty;
        if (tcgetattr(fd, &tty) != 0) return false;

        speed_t speed = map_baud_rate(baud_rate);
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tty.c_oflag &= ~OPOST;
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 1; 

        return tcsetattr(fd, TCSANOW, &tty) == 0;
    }

    void write_frame(const ax25::Frame& frame) {
        std::vector<uint8_t> ax25_data = frame.encode();
        std::vector<uint8_t> kiss_buffer;

        kiss_buffer.push_back(config::KISS_FEND);
        kiss_buffer.push_back(config::KISS_CMD_DATA);

        for (uint8_t b : ax25_data) {
            if (b == config::KISS_FEND) {
                kiss_buffer.push_back(config::KISS_FESC);
                kiss_buffer.push_back(config::KISS_TFEND);
            } else if (b == config::KISS_FESC) {
                kiss_buffer.push_back(config::KISS_FESC);
                kiss_buffer.push_back(config::KISS_TFESC);
            } else {
                kiss_buffer.push_back(b);
            }
        }
        kiss_buffer.push_back(config::KISS_FEND);
        write(fd, kiss_buffer.data(), kiss_buffer.size());
    }

    std::vector<uint8_t> read_ax25_payload() {
        std::vector<uint8_t> frame_buffer;
        uint8_t byte;
        bool in_frame = false, escape_next = false;
        
        while (true) {
            if (read(fd, &byte, 1) > 0) {
                if (byte == config::KISS_FEND) {
                    if (in_frame && !frame_buffer.empty()) {
                        frame_buffer.erase(frame_buffer.begin()); // Remove KISS_CMD_DATA
                        return frame_buffer;
                    }
                    in_frame = true;
                    frame_buffer.clear();
                } else if (in_frame) {
                    if (byte == config::KISS_FESC) {
                        escape_next = true;
                    } else if (escape_next) {
                        if (byte == config::KISS_TFEND) frame_buffer.push_back(config::KISS_FEND);
                        if (byte == config::KISS_TFESC) frame_buffer.push_back(config::KISS_FESC);
                        escape_next = false;
                    } else {
                        frame_buffer.push_back(byte);
                    }
                }
            } else {
                break; // Timeout da porta serial
            }
        }
        return {};
    }
};

// ========================================================================
// 4. APLICAÇÃO PRINCIPAL (CHAT INTERATIVO E SINCRONIZAÇÃO DE ESTADOS)
// ========================================================================
std::mutex cout_mutex; // Mutex para evitar encavalamento no terminal

void send_payload(KissTNC& tnc, ax25::Frame& frame, const std::string& data) {
    std::vector<uint8_t> payload(data.begin(), data.end());
    frame.set_payload(payload);
    tnc.write_frame(frame);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Uso: " << argv[0] << " <serial_dev> <baud> <TARGET_CALL-SSID>\n";
        std::cerr << "Ex:  " << argv[0] << " /tmp/ttyKISS 9600 G2UGK-1\n";
        return 1;
    }

    std::string device = argv[1];
    int baud = std::stoi(argv[2]);
    std::string target_str = argv[3];

    KissTNC tnc;
    if (!tnc.connect(device, baud)) {
        std::cerr << "[-] Erro: Falha ao abrir " << device << "\n";
        return 1;
    }
    
    ax25::Address me("N1CALL", 0);
    ax25::Address target = ax25::Address::from_string(target_str);

    std::cout << "[+] Conectado à TNC em " << device << ".\n";
    std::cout << "[*] Iniciando conexao com " << target_str << "...\n";

    // 1. Enviar SABM (Set Asynchronous Balanced Mode)
    ax25::Frame sabm_frame(target, me);
    sabm_frame.set_control(config::AX25_SABM_CMD);
    tnc.write_frame(sabm_frame);

    // Contadores Atômicos Inteligentes para Sincronização AX.25
    std::atomic<uint8_t> next_ns{0};
    std::atomic<uint8_t> next_nr{0};

    // 2. Thread de Recepção (RX) e Máquina de Estados ATIVA
    std::atomic<bool> rx_running{true};
    std::thread rx_thread([&tnc, &rx_running, &next_ns, &next_nr, &target, &me]() {
        while (rx_running) {
            auto raw_ax25 = tnc.read_ax25_payload();
            if (!raw_ax25.empty()) {
                auto frame = ax25::Frame::decode_payload_only(raw_ax25);
                uint8_t ctrl = frame.get_control();
                
                // --- Sincronizador Dinâmico e ACK Automático ---
                if ((ctrl & ax25::CONTROL_I_FRAME_MASK) == ax25::CONTROL_I_FRAME_BIT) { 
                    // Recebemos um I-Frame (Texto) da BBS
                    uint8_t received_ns = (ctrl >> config::AX25_NS_SHIFT) & config::AX25_SEQ_MASK;
                    uint8_t received_nr = (ctrl >> config::AX25_NR_SHIFT) & config::AX25_SEQ_MASK;
                    
                    uint8_t expected_ns = (received_ns + 1) % config::AX25_SEQ_MODULO;
                    next_nr = expected_ns; // Atualiza nosso N(R)
                    next_ns = received_nr; // Atualiza nosso N(S)
                    
                    // Envia ACK (RR - Receive Ready) para liberar buffer da BBS
                    uint8_t rr_control = (next_nr.load() << config::AX25_NR_SHIFT) | config::AX25_RR_BASE;
                    ax25::Frame ack_frame(target, me);
                    ack_frame.set_control(rr_control);
                    ack_frame.set_pid(ax25::PID_NONE);
                    std::vector<uint8_t> empty_payload;
                    ack_frame.set_payload(empty_payload);
                    tnc.write_frame(ack_frame);
                    
                } else if ((ctrl & ax25::CONTROL_S_FRAME_MASK) == ax25::CONTROL_S_FRAME_BIT) { 
                    // Recebemos um S-Frame (RR/REJ da BBS)
                    uint8_t received_nr = (ctrl >> config::AX25_NR_SHIFT) & config::AX25_SEQ_MASK;
                    next_ns = received_nr; 
                    
                    // Se a BBS solicitar resposta (Poll bit ativo), enviamos RR
                    if (ctrl & config::AX25_PF_BIT) {
                        uint8_t rr_control = (next_nr.load() << config::AX25_NR_SHIFT) | config::AX25_RR_BASE;
                        ax25::Frame ack_frame(target, me);
                        ack_frame.set_control(rr_control);
                        ack_frame.set_pid(ax25::PID_NONE);
                        std::vector<uint8_t> empty_payload;
                        ack_frame.set_payload(empty_payload);
                        tnc.write_frame(ack_frame);
                    }
                }

                auto payload = frame.get_payload();
                if (!payload.empty()) {
                    std::string text(payload.begin(), payload.end());
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cout << "\r[RX] " << text << "\n> " << std::flush;
                }
            }
            usleep(config::POLL_DELAY_US * 10);
        }
    });

    // 3. Loop Interativo (TX)
    std::cout << "[*] Digite a mensagem e pressione Enter. Digite '" << config::CMD_DISCONNECT << "' para desconectar.\n";
    
    while (true) {
        std::string input;
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "> " << std::flush;
        }
        
        std::getline(std::cin, input);

        if (input == config::CMD_DISCONNECT) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "[*] Enviando comando de desconexao (DISC)...\n";
            ax25::Frame disc_frame(target, me);
            disc_frame.set_control(config::AX25_DISC_CMD);
            tnc.write_frame(disc_frame);
            break;
        }

        if (!input.empty()) {
            input += config::CHAR_CR; // Adiciona o Carriage Return explícito
            
            // Lê os contadores estabilizados pela thread RX
            uint8_t ns_to_send = next_ns.load();
            uint8_t nr_to_send = next_nr.load();

            uint8_t control_byte = (nr_to_send << config::AX25_NR_SHIFT) | 
                                   (ns_to_send << config::AX25_NS_SHIFT);
            
            ax25::Frame i_frame(target, me);
            i_frame.set_control(control_byte);
            i_frame.set_pid(ax25::PID_NONE);
            send_payload(tnc, i_frame, input);
            
            // Incrementa assumindo sucesso; se falhar, RX Thread retrocede o next_ns
            next_ns = (ns_to_send + 1) % config::AX25_SEQ_MODULO;
        }
    }

    rx_running = false;
    rx_thread.join();

    // ========================================================================
    // 4. BROADCASTS APRS PÓS-DESCONEXÃO
    // ========================================================================
    std::cout << "[*] Enviando Broadcasts APRS...\n";
    ax25::Address aprs_dest("APRS", 0);
    
    ax25::Frame aprs_loc(aprs_dest, me);
    aprs_loc.add_digipeater(ax25::Address("WIDE1", 1));
    aprs_loc.set_control(ax25::U_FRAME_UI);
    aprs_loc.set_pid(ax25::PID_NONE);
    send_payload(tnc, aprs_loc, "!4903.50N/07201.75W-Teste de Coordenada via N1CALL");

    ax25::Frame aprs_status(aprs_dest, me);
    aprs_status.add_digipeater(ax25::Address("WIDE1", 1));
    aprs_status.set_control(ax25::U_FRAME_UI);
    aprs_status.set_pid(ax25::PID_NONE);
    send_payload(tnc, aprs_status, ">N1CALL BBS is online and accepting connections on VHF");

    std::cout << "[+] Sessao encerrada.\n";
    return 0;
}
