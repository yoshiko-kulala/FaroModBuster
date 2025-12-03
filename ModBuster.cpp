#include <modbus.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <ctime> // 現在時刻の取得

// SQLエラーメッセージを表示する関数
void PrintSQLError(SQLHSTMT hStmt, SQLRETURN ret)
{
    SQLWCHAR sqlState[1024];
    SQLWCHAR message[1024];
    SQLINTEGER nativeError;
    SQLSMALLINT textLength;
    SQLSMALLINT i = 0;

    // 全てのエラー情報を取得して表示
    while (SQLGetDiagRecW(SQL_HANDLE_STMT, hStmt, ++i, sqlState, &nativeError, message, sizeof(message), &textLength) == SQL_SUCCESS)
    {
        std::wcerr << L"SQL Error " << i << L": " << message << L"\nSQL State: " << sqlState << std::endl;
        std::wcerr << L"Native Error Code: " << nativeError << std::endl;
    }
}

// UiDisplayDataにデータを追加
void InsertDataToUiDisplayTableWithPreviousValues(int fanWh)
{
    SQLHENV hEnv;
    SQLHDBC hDbc;
    SQLHSTMT hStmt;
    SQLRETURN ret;

    // ODBCの初期化
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    // SQL Serverへの接続
    SQLWCHAR connStr[] = L"Driver={SQL Server};Server=localhost\\SQLEXPRESS;Database=TestDatabase;Trusted_Connection=yes;";
    ret = SQLDriverConnectW(hDbc, NULL, connStr, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL connection failed." << std::endl;
        return;
    }

    // 直前の行のデータを取得
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    double previousValues[37] = { 0.0 }; // 全列分のデフォルト値を0で初期化

    SQLWCHAR selectQuery[] = L"SELECT TOP 1 * FROM dbo.UiDisplayData ORDER BY id DESC";
    ret = SQLPrepareW(hStmt, selectQuery, SQL_NTS);
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
    {
        ret = SQLExecute(hStmt);
        if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO)
        {
            if (SQLFetch(hStmt) == SQL_SUCCESS)
            {
                for (int i = 1; i <= 37; ++i) // idを除く全列を取得
                {
                    SQLGetData(hStmt, i + 1, SQL_C_DOUBLE, &previousValues[i - 1], sizeof(double), NULL);
                }
            }
        }
    }
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);

    // fanWhを1000で割った値を計算
    double cFanOutput1 = static_cast<double>(fanWh) / 1000.0;
    previousValues[36] = cFanOutput1; // c-fan-output1に新しい値を設定

    // UiDisplayDataに新しい行を挿入
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    SQLWCHAR insertQuery[] = L"INSERT INTO dbo.UiDisplayData "
        L"([Lside10_dust], [Lside10_temp], [Lside10_humi], [Lside10_volt], "
        L"[Lside30_dust], [Lside30_temp], [Lside30_humi], [Lside30_volt], "
        L"[Lside50_dust], [Lside50_temp], [Lside50_humi], [Lside50_volt], "
        L"[Rside10_dust], [Rside10_temp], [Rside10_humi], [Rside10_volt], "
        L"[Rside30_dust], [Rside30_temp], [Rside30_humi], [Rside30_volt], "
        L"[Rside50_dust], [Rside50_temp], [Rside50_humi], [Rside50_volt], "
        L"[invert_dust], [invert_temp], [invert_humi], [invert_volt], "
        L"[centru_dust], [centru_temp], [centru_humi], [centru_volt], "
        L"[c-fan-output1], [CO2-reduction], [CO2-reduction-accum]) "
        L"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    ret = SQLPrepareW(hStmt, insertQuery, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL Prepare failed." << std::endl;
        PrintSQLError(hStmt, ret);
        return;
    }

    // パラメータのバインド
    for (int i = 0; i < 37; ++i)
    {
        SQLBindParameter(hStmt, i + 1, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &previousValues[i], 0, NULL);
    }

    // SQLクエリの実行
    ret = SQLExecute(hStmt);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL Execute failed." << std::endl;
        PrintSQLError(hStmt, ret);
    }
    else
    {
        std::cout << "Inserted data into dbo.UiDisplayData with fanWh / 1000.0 = " << cFanOutput1 << "." << std::endl;
    }

    // ハンドルの解放
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
}


// OutputTableから最新の値を読み取り、Modbusに書き込む
void ReadOutputTableAndWriteToModbus(modbus_t* ctx)
{
    SQLHENV hEnv;
    SQLHDBC hDbc;
    SQLHSTMT hStmt;
    SQLRETURN ret;

    // ODBCの初期化
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    // SQL Serverへの接続
    SQLWCHAR connStr[] = L"Driver={SQL Server};Server=localhost\\SQLEXPRESS;Database=TestDatabase;Trusted_Connection=yes;";
    ret = SQLDriverConnectW(hDbc, NULL, connStr, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL connection failed." << std::endl;
        return;
    }

    // SQLステートメントを準備
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // 最新の行（idが最大の行）を取得
    SQLWCHAR sqlQuery[] = L"SELECT TOP 1 FanOutput, FilterOut, WasherOut FROM dbo.OutputTable ORDER BY id DESC";
    ret = SQLPrepareW(hStmt, sqlQuery, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL Prepare failed." << std::endl;
        PrintSQLError(hStmt, ret);
        return;
    }

    ret = SQLExecute(hStmt);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL Execute failed." << std::endl;
        PrintSQLError(hStmt, ret);
        return;
    }

    int fanOutput = 0;
    bool filterOut = false;
    bool washerOut = false;

    // 結果の取得
    if (SQLFetch(hStmt) == SQL_SUCCESS)
    {
        SQLGetData(hStmt, 1, SQL_C_LONG, &fanOutput, sizeof(fanOutput), NULL);
        SQLGetData(hStmt, 2, SQL_C_BIT, &filterOut, sizeof(filterOut), NULL);
        SQLGetData(hStmt, 3, SQL_C_BIT, &washerOut, sizeof(washerOut), NULL);

        // FanOutputを10倍にする
        int fanOutputScaled = fanOutput * 10;

        // Modbusに書き込む
        if (modbus_write_register(ctx, 350, fanOutputScaled) == -1) {
            std::cerr << "Failed to write FanOutputScaled to #350: " << modbus_strerror(errno) << std::endl;
        }
        if (modbus_write_register(ctx, 304, filterOut) == -1) {
            std::cerr << "Failed to write FilterOut to #304: " << modbus_strerror(errno) << std::endl;
        }
        if (modbus_write_register(ctx, 300, washerOut) == -1) {
            std::cerr << "Failed to write WasherOut to #300: " << modbus_strerror(errno) << std::endl;
        }

        std::cout << "FanOutput (original): " << fanOutput
            << ", FanOutput (scaled): " << fanOutputScaled
            << ", FilterOut: " << filterOut
            << ", WasherOut: " << washerOut << std::endl;
    }

    // ハンドルの解放
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
}


// Modbusから読み取ったデータをSQLテーブルに挿入
void InsertDataToSQL(int fanWh, bool filterFB, bool washerFB)
{
    SQLHENV hEnv;
    SQLHDBC hDbc;
    SQLHSTMT hStmt;
    SQLRETURN ret;

    // ODBCの初期化
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    // SQL Serverへの接続
    SQLWCHAR connStr[] = L"Driver={SQL Server};Server=localhost\\SQLEXPRESS;Database=TestDatabase;Trusted_Connection=yes;";
    ret = SQLDriverConnectW(hDbc, NULL, connStr, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL connection failed." << std::endl;
        return;
    }

    // SQLステートメントを準備
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    // SQLクエリ
    SQLWCHAR sqlQuery[] = L"INSERT INTO dbo.ModbusInputData (FanWh, FilterFB, WasherFB) VALUES (?, ?, ?);";

    ret = SQLPrepareW(hStmt, sqlQuery, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL Prepare failed." << std::endl;
        PrintSQLError(hStmt, ret); // エラーメッセージ表示
        return;
    }

    // パラメータのバインド
    SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &fanWh, 0, NULL);
    SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 0, 0, &filterFB, 0, NULL);
    SQLBindParameter(hStmt, 3, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 0, 0, &washerFB, 0, NULL);

    // SQLクエリの実行
    ret = SQLExecute(hStmt);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
    {
        std::cerr << "SQL Execute failed." << std::endl;
        PrintSQLError(hStmt, ret); // エラーメッセージ表示
    }

    // ハンドルの解放
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
}

int main() {
    // Modbus TCPのコンテキストを作成
    modbus_t* ctx = modbus_new_tcp("127.0.0.1", 502);
    //modbus_t* ctx = modbus_new_tcp("192.168.28.231", 502);
    //modbus_t* ctx = modbus_new_tcp("192.168.28.232", 502);
    if (ctx == nullptr) {
        std::cerr << "Unable to create the libmodbus context" << std::endl;
        return -1;
    }

    // PLCへの接続
    if (modbus_connect(ctx) == -1) {
        std::cerr << "Connection failed: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        return -1;
    }

    // タイムアウトの設定
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    modbus_set_response_timeout(ctx, timeout.tv_sec, timeout.tv_usec);
    modbus_set_slave(ctx, 1); // デバイスIDを設定

    // 定期的なModbusデータのチェック
    while (true) {
        uint16_t registers[2];
        int fanWh = 0;
        bool filterFB = false;
        bool washerFB = false;

        // #200, #201 から32ビットの値を読み取る
        if (modbus_read_registers(ctx, 200, 2, registers) != -1) {
            fanWh = (registers[0] << 16) | registers[1];
            // UiDisplayDataにデータを挿入
            InsertDataToUiDisplayTableWithPreviousValues(fanWh);
        }

        // #302 からフィルタステータスを読み取る
        uint16_t filterValue;
        if (modbus_read_registers(ctx, 302, 1, &filterValue) != -1) {
            filterFB = filterValue & 0x01;
        }

        // #304 から洗浄機ステータスを読み取る
        uint16_t washerValue;
        if (modbus_read_registers(ctx, 304, 1, &washerValue) != -1) {
            washerFB = washerValue & 0x01;
        }

        // SQLにデータを挿入
        InsertDataToSQL(fanWh, filterFB, washerFB);

        // OutputTableから最新のデータを取得してModbusに書き込む
        ReadOutputTableAndWriteToModbus(ctx);

        // 1秒待機
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // PLCとの接続を閉じる
    modbus_close(ctx);
    modbus_free(ctx);

    return 0;
}