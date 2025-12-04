#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstdlib>      // system("cls")
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstring>      // std::memcpy
#include <cstdio>       // _popen, _pclose
#include <thread>       // std::this_thread::sleep_for
#include <cerrno>       // errno

extern "C" {
#include "libmodbus/modbus.h"
}

// ===== 設定用 define =====

// Modbus 接続先
#define MODBUS_SERVER_IP   "192.168.3.201"
#define MODBUS_SERVER_PORT 502
#define MODBUS_SLAVE_ID    1

// 読み取り範囲
#define MODBUS_READ_START_ADDR  200
#define MODBUS_READ_COUNT       200

// 1回の読み取り上限（このPLCはツールで64レジスタ読んでいたので、64に固定）
#define MY_MAX_READ_REGS        64

// 表示する範囲（今回は読み取り範囲と同じにしておく）
#define DISPLAY_START_ADDR      200
#define DISPLAY_COUNT           200
#define DISPLAY_COLS            8   // 1行に表示するレジスタ数

// トークン・API
#define TOKEN_FILE_PATH  "C:\\Users\\Farosystem\\FaroSystem\\current_token"
#define PANEL_ID         2
#define API_URL          "https://api.faro-mcm.com/api/modbus/transmission"
#define CURL_LOG_PATH    "C:\\Users\\Farosystem\\FaroSystem\\modbus_curl.log"

// 周期（ミリ秒）
#define MODBUS_SAMPLE_INTERVAL_MS   500     // 0.5sごとに Modbus 読み取り & コンソール表示
#define CURL_SEND_INTERVAL_MS       30000   // 30sごとに curl 送信
#define TIME_WRITE_INTERVAL_MS      10000   // 10sごとに時刻を書き込み

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
            int index = addr + c;
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

// 2つの16bitレジスタ（low, high）から 32bit 整数を作る（unsigned）
static uint32_t make_u32_from_registers(const uint16_t* regs, int low_addr)
{
    uint16_t low = regs[low_addr];
    uint16_t high = regs[low_addr + 1];
    uint32_t value = static_cast<uint32_t>(low)
        | (static_cast<uint32_t>(high) << 16);
    return value;
}

// 2つのレジスタから「0 or 1」のエラー値を作る（1以上は1）
static int make_error_flag_from_registers(const uint16_t* regs, int low_addr)
{
    uint32_t raw = make_u32_from_registers(regs, low_addr);
    return (raw > 0) ? 1 : 0;
}

// JSON文字列を生成（Modbusレジスタから）
// ※ 測定値はすべて uint32_t 整数として扱う
static std::string build_json_payload(const uint16_t* regs)
{
    std::string timestamp = current_timestamp_utc_iso8601();

    // --- measurements (uint32_t) ---
    uint32_t lAfSupplyVolume = make_u32_from_registers(regs, 200);
    uint32_t lAfSupplyVolumeIntegral = make_u32_from_registers(regs, 202);
    uint32_t hAfSupplyVolume = make_u32_from_registers(regs, 204);
    uint32_t hAfSupplyVolumeIntegral = make_u32_from_registers(regs, 206);
    uint32_t concreteSupplyVolume = make_u32_from_registers(regs, 208);
    uint32_t concreteSupplyVolumeIntegral = make_u32_from_registers(regs, 210);
    uint32_t lHRatio = make_u32_from_registers(regs, 212);
    uint32_t lhConcreteRatio = make_u32_from_registers(regs, 214);

    // --- errors (0 or 1) ---
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
        << "\"lAfSupplyVolume\":" << static_cast<uint64_t>(lAfSupplyVolume) << ","
        << "\"lAfSupplyVolumeIntegral\":" << static_cast<uint64_t>(lAfSupplyVolumeIntegral) << ","
        << "\"hAfSupplyVolume\":" << static_cast<uint64_t>(hAfSupplyVolume) << ","
        << "\"hAfSupplyVolumeIntegral\":" << static_cast<uint64_t>(hAfSupplyVolumeIntegral) << ","
        << "\"concreteSupplyVolume\":" << static_cast<uint64_t>(concreteSupplyVolume) << ","
        << "\"concreteSupplyVolumeIntegral\":" << static_cast<uint64_t>(concreteSupplyVolumeIntegral) << ","
        << "\"lHRatio\":" << static_cast<uint64_t>(lHRatio) << ","
        << "\"lhConcreteRatio\":" << static_cast<uint64_t>(lhConcreteRatio)
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

// curl を使ってサーバーに POST ＆ ログ記録
static void send_payload_via_curl(const uint16_t* regs)
{
    std::string token = read_token_from_file(TOKEN_FILE_PATH);
    if (token.empty()) {
        std::cerr << "[WARN] Token is empty. Skip sending.\n";
        return;
    }

    // 送信する JSON を生成
    std::string json = build_json_payload(regs);

    // コマンドライン用に JSON をエスケープ
    std::string json_escaped = escape_for_cmd_double_quoted(json);

    // curl コマンド組み立て（HTTPコードを最後に出力させる）
    std::ostringstream cmd;
    cmd << "curl -sS -w \"\\nHTTP_CODE:%{http_code}\\n\" \"" << API_URL << "\""
        << " -H \"X-Panel-Auth: " << PANEL_ID << ":" << token << "\""
        << " -H \"Content-Type: application/json\""
        << " -d \"" << json_escaped << "\"";

    std::string cmd_str = cmd.str();

    // curl の標準出力を取得
    std::string curl_output;
    FILE* pipe = _popen(cmd_str.c_str(), "r");
    if (!pipe) {
        std::cerr << "[ERROR] _popen failed for curl.\n";
        return;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        curl_output += buffer;
    }
    int ret = _pclose(pipe);

    // 時刻（ロギング用）
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &t);
#else
    local_tm = *std::localtime(&t);
#endif
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &local_tm);

    // === ログファイルに追記 ===
    std::ofstream log(CURL_LOG_PATH, std::ios::app);
    if (!log) {
        std::cerr << "[WARN] Failed to open log file: " << CURL_LOG_PATH << "\n";
    }
    else {
        log << "==============================\n";
        log << "TIME: " << timebuf << "\n";
        // token はログに残さない
        log << "REQUEST JSON:\n" << json << "\n\n";
        log << "CURL OUTPUT (ret=" << ret << "):\n"
            << curl_output << "\n";
    }

    // コンソールには軽いサマリ表示
    std::cout << "[INFO] Payload sent. curl ret=" << ret << "\n";

    // HTTP_CODE だけ拾えたら表示
    auto pos = curl_output.rfind("HTTP_CODE:");
    if (pos != std::string::npos) {
        std::string code = curl_output.substr(pos + 10);
        // 改行・空白をトリム
        code.erase(std::remove_if(code.begin(), code.end(),
            [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == ' '; }),
            code.end());
        if (!code.empty()) {
            std::cout << "[INFO] HTTP_CODE=" << code << "\n";
        }
    }
}

// 10sごとに PC 時刻をスレーブへ書き込む（#242〜253, #262）
static bool write_pc_time_to_slave(modbus_t* ctx, uint16_t* regs)
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

    int year = local_tm.tm_year + 1900; // 1900基準
    int month = local_tm.tm_mon + 1;     // 0-11 -> 1-12
    int day = local_tm.tm_mday;
    int hour = local_tm.tm_hour;
    int minute = local_tm.tm_min;
    int second = local_tm.tm_sec;

    // 下位ワードに値、上位ワードは 0 のペアをまとめて書く
    uint16_t buf[12];
    buf[0] = static_cast<uint16_t>(year);
    buf[1] = 0;
    buf[2] = static_cast<uint16_t>(month);
    buf[3] = 0;
    buf[4] = static_cast<uint16_t>(day);
    buf[5] = 0;
    buf[6] = static_cast<uint16_t>(hour);
    buf[7] = 0;
    buf[8] = static_cast<uint16_t>(minute);
    buf[9] = 0;
    buf[10] = static_cast<uint16_t>(second);
    buf[11] = 0;

    // #242〜#253 に 12 レジスタ分書き込み
    int rc = modbus_write_registers(ctx, 242, 12, buf);
    if (rc == -1) {
        std::cerr << "[WARN] Failed to write time registers (242-253): "
            << modbus_strerror(errno) << "\n";
        return false;
    }

    // #262 に 0 を書き込み
    rc = modbus_write_register(ctx, 262, 0);
    if (rc == -1) {
        std::cerr << "[WARN] Failed to write register 262: "
            << modbus_strerror(errno) << "\n";
        return false;
    }

    // ローカルコピー regs[] も更新しておく（表示・JSONで使えるように）
    regs[242] = buf[0];
    regs[243] = buf[1];
    regs[244] = buf[2];
    regs[245] = buf[3];
    regs[246] = buf[4];
    regs[247] = buf[5];
    regs[248] = buf[6];
    regs[249] = buf[7];
    regs[250] = buf[8];
    regs[251] = buf[9];
    regs[252] = buf[10];
    regs[253] = buf[11];
    regs[262] = 0;

    return true;
}

// 最大 MY_MAX_READ_REGS ごとに分割して連続領域をまとめて読む
static bool read_registers_chunked(modbus_t* ctx,
    uint16_t* regs,
    int start_addr,
    int nb)
{
    int addr = start_addr;
    int remaining = nb;

    while (remaining > 0) {
        int to_read = remaining;
        if (to_read > MY_MAX_READ_REGS) {
            to_read = MY_MAX_READ_REGS;
        }

        int rc = modbus_read_registers(ctx, addr, to_read, &regs[addr]);
        if (rc == -1) {
            std::cerr << "[ERROR] modbus_read_registers failed at addr "
                << addr << " count " << to_read << ": "
                << modbus_strerror(errno) << "\n";
            return false;
        }
        if (rc != to_read) {
            std::cerr << "[WARN] modbus_read_registers read " << rc
                << " registers (expected " << to_read
                << ") at addr " << addr << "\n";
            // 必要なら return false にしてもよい
        }

        addr += to_read;
        remaining -= to_read;
    }

    return true;
}

// 0.5sごとのスナップショット表示
static void print_snapshot(const uint16_t* regs)
{
    system("cls");
    print_now_local();
    print_registers(regs, DISPLAY_START_ADDR, DISPLAY_COUNT);
    std::cout << "\n(PC is Modbus TCP MASTER. "
        "Slave " << MODBUS_SERVER_IP << " ID=" << MODBUS_SLAVE_ID
        << " registers " << MODBUS_READ_START_ADDR << "-"
        << (MODBUS_READ_START_ADDR + MODBUS_READ_COUNT - 1)
        << " read in chunks of " << MY_MAX_READ_REGS << ".)\n";
}

// ===== メイン処理 =====
int main()
{
    // Modbus TCP マスターとして初期化
    modbus_t* ctx = modbus_new_tcp(MODBUS_SERVER_IP, MODBUS_SERVER_PORT);
    if (ctx == nullptr) {
        std::cerr << "Unable to create the libmodbus context\n";
        return -1;
    }

    // 古い libmodbus: set_response_timeout は (ctx, sec, usec) 形式
    if (modbus_set_response_timeout(ctx, 1, 0) == -1) {
        std::cerr << "Failed to set response timeout: "
            << modbus_strerror(errno) << "\n";
        // ここで終了してもいいし、そのまま続行してもよい
    }

    // Slave ID 設定（ツールログより 0x01 でOK）
    if (modbus_set_slave(ctx, MODBUS_SLAVE_ID) == -1) {
        std::cerr << "Failed to set slave ID: " << modbus_strerror(errno) << "\n";
        modbus_free(ctx);
        return -1;
    }

    // レジスタ読み取り用バッファ（0〜399 用・全部 0 初期化）
    uint16_t regs[400];
    for (int i = 0; i < 400; ++i) {
        regs[i] = 0;
    }

    using steady_clock = std::chrono::steady_clock;

    while (true) {
        std::cout << "Connecting to Modbus TCP slave "
            << MODBUS_SERVER_IP << ":" << MODBUS_SERVER_PORT
            << " (ID=" << MODBUS_SLAVE_ID << ")...\n";

        if (modbus_connect(ctx) == -1) {
            std::cerr << "Connection failed: " << modbus_strerror(errno) << "\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;   // 再接続を試みる
        }

        std::cout << "Connected.\n";

        auto next_sample_time = steady_clock::now();
        auto next_curl_time = steady_clock::now();
        auto next_time_write = steady_clock::now();

        bool need_reconnect = false;

        while (!need_reconnect) {
            auto now = steady_clock::now();

            // 0.5sごとに Modbus 読み取り & 表示
            if (now >= next_sample_time) {
                if (!read_registers_chunked(ctx,
                    regs,
                    MODBUS_READ_START_ADDR,
                    MODBUS_READ_COUNT)) {
                    need_reconnect = true;
                }
                else {
                    print_snapshot(regs);
                    next_sample_time = now + std::chrono::milliseconds(MODBUS_SAMPLE_INTERVAL_MS);
                }
            }

            if (need_reconnect) break;

            // 30sごとに curl 送信
            if (now >= next_curl_time) {
                send_payload_via_curl(regs);
                next_curl_time = now + std::chrono::milliseconds(CURL_SEND_INTERVAL_MS);
            }

            // 10sごとに PC 時刻を書き込み
            if (now >= next_time_write) {
                if (!write_pc_time_to_slave(ctx, regs)) {
                    std::cerr << "[WARN] Failed to write PC time to slave.\n";
                    // 書き込み失敗しても即座に再接続まではしない（必要に応じて変えてOK）
                }
                next_time_write = now + std::chrono::milliseconds(TIME_WRITE_INTERVAL_MS);
            }

            // 負荷軽減のため少し sleep
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cerr << "Connection lost. Closing and will retry...\n";
        modbus_close(ctx);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    modbus_close(ctx);
    modbus_free(ctx);

    return 0;
}
