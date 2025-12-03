#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstdlib>      // system("cls"), system()
#include <cstdint>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstring>      // std::memcpy

extern "C" {
#include "libmodbus/modbus.h"
}

// ===== 設定用 define =====
#define DISPLAY_START_ADDR      200
#define DISPLAY_COUNT           200
#define DISPLAY_COLS            8   // 1行に表示するレジスタ数

#define TOKEN_FILE_PATH  "C:\\Users\\Farosystem\\FaroSystem\\current_token"
#define PANEL_ID         2
#define API_URL          "https://api.faro-mcm.com/api/modbus/transmission"

// 周期（ミリ秒）
#define MODBUS_SAMPLE_INTERVAL_MS  500     // 0.5sごとに表示
#define CURL_SEND_INTERVAL_MS      30000   // 30sごとにcurl送信

// ===== ヘルパ関数群 =====

// レジスタ一覧を整形して出力
static void print_registers(const uint16_t* regs, int start_addr, int count)
{
    int end_addr = start_addr + count;

    std::cout << "Holding Registers " << start_addr << " - " << (end_addr - 1)
        << " (total " << count << ")\n\n";

    for (int addr = start_addr; addr < end_addr; addr += DISPLAY_COLS) {
        std::cout << std::setw(4) << addr << ": ";

        for (int c = 0; c < DISPLAY_COLS && (addr + c) < end_addr; ++c) {
            int index = addr + c;  // アドレス = インデックス
            std::cout << std::setw(6) << regs[index];
        }
        std::cout << '\n';
    }
}

// 現在時刻を「YYYY-MM-DD HH:MM:SS」で出す（コンソール用）
static void print_now_local()
{
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &t);
#else
    local_tm = *std::localtime(&t);
#endif
    std::cout << "Last update: "
        << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
        << "\n\n";
}

// 現在時刻を UTC で「YYYY-MM-DDTHH:MM:SSZ」形式にする（JSON 用）
static std::string current_timestamp_utc_iso8601()
{
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    tm_utc = *std::gmtime(&t);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

// トークンファイルを読み込む（1行目をそのまま使用）
static std::string read_token_from_file(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "[WARN] Failed to open token file: " << path << "\n";
        return "";
    }

    std::string line;
    std::getline(ifs, line);

    // 改行や空白をトリム
    line.erase(std::remove_if(line.begin(), line.end(),
        [](unsigned char ch) {
            return ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t';
        }),
        line.end());
    return line;
}

// 2つの16bitレジスタ（low, high）から 32bit 整数を作る
static uint32_t make_u32_from_registers(const uint16_t* regs, int low_addr)
{
    uint16_t low = regs[low_addr];
    uint16_t high = regs[low_addr + 1];
    uint32_t value = static_cast<uint32_t>(low)
        | (static_cast<uint32_t>(high) << 16);
    return value;
}

// 2つのレジスタから float を取り出す（低位ワードが先、little endian）
static float make_float_from_registers(const uint16_t* regs, int low_addr)
{
    uint32_t raw = make_u32_from_registers(regs, low_addr);
    float f;
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 4 bytes");
    std::memcpy(&f, &raw, sizeof(float));
    return f;
}

// 2つのレジスタから「0 or 1」のエラー値を作る（1以上は1）
static int make_error_flag_from_registers(const uint16_t* regs, int low_addr)
{
    uint32_t raw = make_u32_from_registers(regs, low_addr);
    return (raw > 0) ? 1 : 0;
}

// JSON文字列を生成（Modbusレジスタから）
static std::string build_json_payload(const uint16_t* regs)
{
    std::string timestamp = current_timestamp_utc_iso8601();

    // --- measurements ---
    float lAfSupplyVolume = make_float_from_registers(regs, 200);
    float lAfSupplyVolumeIntegral = make_float_from_registers(regs, 202);
    float hAfSupplyVolume = make_float_from_registers(regs, 204);
    float hAfSupplyVolumeIntegral = make_float_from_registers(regs, 206);
    float concreteSupplyVolume = make_float_from_registers(regs, 208);
    float concreteSupplyVolumeIntegral = make_float_from_registers(regs, 210);
    float lHRatio = make_float_from_registers(regs, 212);
    float lhConcreteRatio = make_float_from_registers(regs, 214);

    // --- errors ---
    int natomicLsaPumpError = make_error_flag_from_registers(regs, 216);
    int lsaFlowDecrease = make_error_flag_from_registers(regs, 218);
    int lsaTankLevelLow = make_error_flag_from_registers(regs, 220);
    int lsaTankLevelVeryLow = make_error_flag_from_registers(regs, 222);
    int invError1 = make_error_flag_from_registers(regs, 224);
    int invError2 = make_error_flag_from_registers(regs, 226);
    int invError3 = make_error_flag_from_registers(regs, 228);
    int invError4 = make_error_flag_from_registers(regs, 230);
    int invError5 = make_error_flag_from_registers(regs, 232);
    int invError6 = make_error_flag_from_registers(regs, 234);
    int invError7 = make_error_flag_from_registers(regs, 236);
    int invError8 = make_error_flag_from_registers(regs, 238);
    int invError9 = make_error_flag_from_registers(regs, 240);

    std::ostringstream oss;
    oss << "{"
        << "\"panelId\":" << PANEL_ID << ","
        << "\"plcTimestamp\":\"" << timestamp << "\","
        << "\"pcHealthCheck\":1,"

        << "\"measurements\":{"
        << "\"lAfSupplyVolume\":" << lAfSupplyVolume << ","
        << "\"lAfSupplyVolumeIntegral\":" << lAfSupplyVolumeIntegral << ","
        << "\"hAfSupplyVolume\":" << hAfSupplyVolume << ","
        << "\"hAfSupplyVolumeIntegral\":" << hAfSupplyVolumeIntegral << ","
        << "\"concreteSupplyVolume\":" << concreteSupplyVolume << ","
        << "\"concreteSupplyVolumeIntegral\":" << concreteSupplyVolumeIntegral << ","
        << "\"lHRatio\":" << lHRatio << ","
        << "\"lhConcreteRatio\":" << lhConcreteRatio
        << "},"

        << "\"errors\":{"
        << "\"natomicLsaPumpError\":" << natomicLsaPumpError << ","
        << "\"lsaFlowDecrease\":" << lsaFlowDecrease << ","
        << "\"lsaTankLevelLow\":" << lsaTankLevelLow << ","
        << "\"lsaTankLevelVeryLow\":" << lsaTankLevelVeryLow << ","
        << "\"invError1\":" << invError1 << ","
        << "\"invError2\":" << invError2 << ","
        << "\"invError3\":" << invError3 << ","
        << "\"invError4\":" << invError4 << ","
        << "\"invError5\":" << invError5 << ","
        << "\"invError6\":" << invError6 << ","
        << "\"invError7\":" << invError7 << ","
        << "\"invError8\":" << invError8 << ","
        << "\"invError9\":" << invError9
        << "}"
        << "}";

    return oss.str();
}

// system() で curl を叩くために JSON 内の " を \" にエスケープ
static std::string escape_for_cmd_double_quoted(const std::string& s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (char ch : s) {
        if (ch == '\"') {
            out += "\\\"";
        }
        else {
            out += ch;
        }
    }
    return out;
}

// curl を使ってサーバーに POST
static void send_payload_via_curl(const uint16_t* regs)
{
    std::string token = read_token_from_file(TOKEN_FILE_PATH);
    if (token.empty()) {
        std::cerr << "[WARN] Token is empty. Skip sending.\n";
        return;
    }

    std::string json = build_json_payload(regs);
    std::string json_escaped = escape_for_cmd_double_quoted(json);

    std::ostringstream cmd;
    cmd << "curl -s -X POST \"" << API_URL << "\""
        << " -H \"X-Panel-Auth: " << PANEL_ID << ":" << token << "\""
        << " -H \"Content-Type: application/json\""
        << " -d \"" << json_escaped << "\"";

    std::string cmd_str = cmd.str();

    std::cout << "[INFO] Sending payload via curl...\n";
    int ret = system(cmd_str.c_str());
    if (ret != 0) {
        std::cerr << "[WARN] curl command returned non-zero: " << ret << "\n";
    }
    else {
        std::cout << "[INFO] Payload sent successfully.\n";
    }
}

// 0.5sごとの表示
static void print_snapshot(const uint16_t* regs)
{
    system("cls");
    print_now_local();
    print_registers(regs, DISPLAY_START_ADDR, DISPLAY_COUNT);
    std::cout << "\n(PC is Modbus TCP slave. "
        "Master can read/write registers 0–399; "
        "this view shows 200–399 only.)\n";
}

// ===== メイン処理 =====
int main()
{
    // --- Modbus TCP スレーブ(サーバ)として初期化 ---
    modbus_t* ctx = modbus_new_tcp(nullptr, 502); // 0.0.0.0:502
    if (ctx == nullptr) {
        std::cerr << "Unable to create the libmodbus context\n";
        return -1;
    }

    // （レスポンスタイムアウトはとりあえずデフォルトのままでもOK）

    modbus_set_slave(ctx, 1);

    // レジスタマッピング: holding registers 0〜399
    modbus_mapping_t* mb_mapping = modbus_mapping_new(
        0,      // nb_coil_status
        0,      // nb_input_status
        400,    // nb_holding_registers
        0       // nb_input_registers
    );

    if (mb_mapping == nullptr) {
        std::cerr << "Failed to allocate the mapping: "
            << modbus_strerror(errno) << "\n";
        modbus_free(ctx);
        return -1;
    }

    for (int i = 0; i < 400; ++i) {
        mb_mapping->tab_registers[i] = 0;
    }

    // TCP リッスンソケット
    int server_socket = modbus_tcp_listen(ctx, 1);
    if (server_socket == -1) {
        std::cerr << "Unable to listen TCP: " << modbus_strerror(errno) << "\n";
        modbus_mapping_free(mb_mapping);
        modbus_free(ctx);
        return -1;
    }

    std::cout << "Modbus TCP slave started on port 502.\n"
        << "Waiting for master connection...\n";

    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

    using steady_clock = std::chrono::steady_clock;

    while (true) {
        if (modbus_tcp_accept(ctx, &server_socket) == -1) {
            std::cerr << "Failed to accept a connection: "
                << modbus_strerror(errno) << "\n";
            break;
        }

        std::cout << "Master connected.\n";

        // 周期タイマ初期化
        auto next_sample_time = steady_clock::now();
        auto next_curl_time = steady_clock::now();

        // クライアントソケット取得
        int client_socket = modbus_get_socket(ctx);

        while (true) {
            // select で 100ms 待ちつつ、受信有無を見る
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(client_socket, &rfds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000; // 100ms

            int sel = select(client_socket + 1, &rfds, NULL, NULL, &tv);
            if (sel == -1) {
                std::cerr << "[ERROR] select() failed.\n";
                break;
            }

            // データが来ていたら受信→応答
            if (sel > 0 && FD_ISSET(client_socket, &rfds)) {
                int rc = modbus_receive(ctx, query);
                if (rc == -1) {
                    std::cerr << "Connection closed or error: "
                        << modbus_strerror(errno) << "\n";
                    break;  // 接続終了 → 次のクライアント待ちへ
                }

                if (modbus_reply(ctx, query, rc, mb_mapping) == -1) {
                    std::cerr << "Failed to send reply: "
                        << modbus_strerror(errno) << "\n";
                    break;
                }
                // mb_mapping->tab_registers はここで更新される
            }

            auto now = steady_clock::now();

            // 0.5sごとにスナップショット表示
            if (now >= next_sample_time) {
                print_snapshot(mb_mapping->tab_registers);
                next_sample_time = now + std::chrono::milliseconds(MODBUS_SAMPLE_INTERVAL_MS);
            }

            // 30sごとにcurl送信
            if (now >= next_curl_time) {
                send_payload_via_curl(mb_mapping->tab_registers);
                next_curl_time = now + std::chrono::milliseconds(CURL_SEND_INTERVAL_MS);
            }
        }

        std::cout << "Master disconnected. Waiting for new connection...\n";
    }

    modbus_mapping_free(mb_mapping);
    modbus_close(ctx);
    modbus_free(ctx);

    return 0;
}
