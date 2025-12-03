#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstdlib>      // system("cls")
#include <cstdint>

extern "C" {
#include "libmodbus/modbus.h"
}

static void print_registers(const uint16_t* regs, int start_addr, int count)
{
    constexpr int COLS = 8;  // 1 行あたりのレジスタ数（横方向）
    int end_addr = start_addr + count;

    std::cout << "Holding Registers " << start_addr << " - " << (end_addr - 1)
        << " (total " << count << ")\n\n";

    for (int addr = start_addr; addr < end_addr; addr += COLS) {
        // 行の先頭に先頭アドレスを表示
        std::cout << std::setw(4) << addr << ": ";

        for (int c = 0; c < COLS && (addr + c) < end_addr; ++c) {
            int index = addr + c;  // libmodbus の mapping はアドレス = インデックス
            std::cout << std::setw(6) << regs[index];
        }
        std::cout << '\n';
    }
}

// 現在時刻を「YYYY-MM-DD HH:MM:SS」形式で出力するヘルパ
static void print_now()
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

int main()
{
    // --- Modbus TCP スレーブ(サーバ)として初期化 ---
    // 第1引数はバインドする IP。nullptr や "0.0.0.0" で全インターフェースにバインド
    modbus_t* ctx = modbus_new_tcp(nullptr, 502);
    if (ctx == nullptr) {
        std::cerr << "Unable to create the libmodbus context\n";
        return -1;
    }

    // レスポンスタイムアウト（お好みで）
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    modbus_set_response_timeout(ctx, timeout.tv_sec, timeout.tv_usec);

    // スレーブ ID（必要に応じて）
    modbus_set_slave(ctx, 1);

    // --- レジスタマッピングを作成 ---
    // coils, discrete inputs は使わないので 0。
    // holding registers は 0〜399 の 400 個確保し、そのうち 200〜399 を表示に使用。
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

    // 初期値は 0 で OK（必要ならここで初期値を入れてもよい）
    for (int i = 0; i < 400; ++i) {
        mb_mapping->tab_registers[i] = 0;
    }

    // --- TCP リッスンソケットを作成 ---
    int server_socket = modbus_tcp_listen(ctx, 1);  // 同時接続 1
    if (server_socket == -1) {
        std::cerr << "Unable to listen TCP: " << modbus_strerror(errno) << "\n";
        modbus_mapping_free(mb_mapping);
        modbus_free(ctx);
        return -1;
    }

    std::cout << "Modbus TCP slave started on port 502.\n"
        << "Waiting for master connection...\n";

    // クエリバッファ
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

    while (true) {
        // クライアント接続を待つ
        if (modbus_tcp_accept(ctx, &server_socket) == -1) {
            std::cerr << "Failed to accept a connection: "
                << modbus_strerror(errno) << "\n";
            break;
        }

        std::cout << "Master connected.\n";

        // この接続が続く限りリクエストを処理
        while (true) {
            int rc = modbus_receive(ctx, query);
            if (rc == -1) {
                std::cerr << "Connection closed or error: "
                    << modbus_strerror(errno) << "\n";
                break;  // 接続終了 → 次のクライアント待ちへ
            }

            // 受信したリクエストに対して応答、書き込み要求なら mb_mapping が更新される
            if (modbus_reply(ctx, query, rc, mb_mapping) == -1) {
                std::cerr << "Failed to send reply: "
                    << modbus_strerror(errno) << "\n";
                break;
            }

            // --- ここで 200〜399 の 200レジスタを画面に表示 ---
            // 画面クリアして毎回「状態ビュー」を作り直す
            system("cls");

            print_now();
            print_registers(mb_mapping->tab_registers, 200, 200);

            std::cout << "\n(PC is Modbus TCP slave. "
                "Master can read/write registers 0–399; "
                "this view shows 200–399 only.)\n";
        }

        std::cout << "Master disconnected. Waiting for new connection...\n";
    }

    // 後片付け（通常はここまで来ないが念のため）
    modbus_mapping_free(mb_mapping);
    modbus_close(ctx);
    modbus_free(ctx);

    return 0;
}
