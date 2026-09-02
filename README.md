# M5Stack Core2 + 3 SwitchBot Plug Mini + InfluxDB

これは M5Stack Core2 から SwitchBot Plug Mini 3台を BLE で監視し、InlfuxDB 2.x に送信するための PlatformIO プロジェクトです。

## 概要

このプロジェクトは、単一プラグ監視の延長として 3 台同時監視に対応しています。各プラグの電力・状態・RSSI を定期的に取得し、JST を基準にした 30分単位の積算値と日積算値を InfluxDB に書き込みます。

## 送信する InfluxDB の計測値

- `switchbot_ble`
  - 各スキャン周期ごとの瞬時電力
  - BLE RSSI
  - ON/OFF 状態
  - シーケンス番号
  - タイムスタンプ

- `switchbot_energy_30m`
  - JST の各 30 分窓ごとのエネルギー積算
  - 例: `hh:00-hh:30` または `hh:30-hh:00`

- `switchbot_energy_day`
  - JST の 00:00 以降の累積エネルギー
  - 30 分境界ごとに更新

3 台のデバイスは同じ計測名を共有し、`device_id` タグで識別します。

## 必要な準備

1. `include/secrets.h` を編集して Wi-Fi と InfluxDB の接続情報を設定します。
2. `src/main.cpp` 内の Plug-2 / Plug-3 の MAC アドレスを実際の端末に合わせて修正します。
3. PlatformIO でビルド・アップロードを行います。
4. シリアルモニタを 115200 baud で確認します。

最初のデバイスは `84:FC:E6:B4:9D:E6` が予め設定されています。

## ビルド前の注意

- `include/secrets.h` には Wi-Fi SSID / パスワード、InfluxDB の URL / Token を設定してください。
- BLE で検出するデバイス数は 3 台前提です。
- `DEVICE_CONFIG` の MAC アドレスが未設定のままだと、対象端末がスキャン対象になりません。

## 日次積算と再起動時の挙動

日次累算値は、完了した 30 分窓ごとに ESP32 の NVS に保存されます。Core2 が再起動しても、当日分で既に完了した 30 分の積算は復元されます。

一方で、現在進行中の 30 分窓は補完されません。再起動後は `coverage_complete=false` で、当日の計測に穴があることを示します。

完全に欠損のない日次値を維持したい場合は、深夜跨ぎで Core2 を停止させないようにしてください。将来的には、InfluxDB 側の最新積算値から部分的な復元を行う改良も可能です。

## プロジェクト構成

- `src/main.cpp` : BLE スキャン・収集・InfluxDB 書き込みロジック
- `include/secrets.h` : Wi-Fi / InfluxDB の秘密情報
- `platformio.ini` : PlatformIO の設定
- `README.md` : 利用方法の説明

## main.cpp の構成と解説

`src/main.cpp` は以下の 11 個のセクションで構成されています：

### 1. User configuration（ユーザー設定）
- **GATEWAY_ID**: Core2 のゲートウェイ識別子（デフォルト: `Core2-1`）
- **DEVICE_CONFIG**: 監視対象の 3 台の SwitchBot Plug Mini の MAC アドレスとラベル
  - Plug-1 は `84:FC:E6:B4:9D:E6` で固定
  - Plug-2 / Plug-3 は `00:00:00:00:00:00` で、実際の MAC に置き換える
- **タイミング定数**:
  - `CYCLE_MS = 10000`: 1 スキャンサイクル = 10 秒
  - `BLE_SCAN_SECONDS = 4`: 各サイクルの BLE スキャン時間 = 4 秒
  - `MAX_VALID_GAP_SEC = 120`: 電力補間の最大許容ギャップ = 120 秒（超える場合は補間しない）

### 2. Runtime structures（実行時データ構造）
- **Sample 構造体**: 1 回の BLE スキャンで取得した瞬時データ
  - `valid`, `powerW`, `rssi`, `stateOn`, `sequence`, `epoch`
- **DeviceState 構造体**: 各デバイスの状態を保持
  - `pending`: 現在スキャン中のサンプル
  - `haveCurrent / currentPowerW / currentRssi` など: 直近の計測値
  - `lastPowerW / lastEpoch`: 前回のサンプル（積分用）
  - `halfStart / halfWh / halfIntegratedSec`: 30 分窓の集計
  - `dayStart / dayWh / dayIntegratedSec`: 日次の集計

### 3. Time helpers (JST)（時間計算ヘルパー）
JST タイムゾーンで動作する時間計算関数：
- `getHalfHourStart()`: 指定時刻の 30 分区切りの開始時刻を計算
- `getLocalMidnight()`: 指定時刻の当日の 00:00:00 を計算
- `formatLocalTime()`: UNIX timestamp を "YYYY-MM-DD HH:MM:SS" に変換

### 4. Device/MAC helpers（MAC 処理ヘルパー）
- `macToDeviceId()`: MAC アドレス "XX:XX:XX:XX:XX:XX" を "XXXXXXXXXXXXXX"（コロン除外、大文字）に変換
- `payloadMacToString()`: BLE ペイロードのバイト列を MAC アドレス文字列に変換
- `findConfiguredDevice()`: 指定の MAC アドレスが `DEVICE_CONFIG` に存在するかを調べ、インデックスを返す

### 5. BLE advertisement parser（BLE 広告パーサー）
- **PlugAdvertisedCallbacks クラス**: BLE デバイスの広告パケットを受信・解析
- **SwitchBot Plug Mini メーカーデータフォーマット**（全 12 バイト）:
  - バイト 0-1: Company ID (0x0969、リトルエンディアン対応)
  - バイト 2-7: MAC アドレス
  - バイト 8: シーケンス番号
  - バイト 9: 状態フラグ (bit 7 = ON/OFF)
  - バイト 10: Wi-Fi RSSI
  - バイト 10-11: 15 ビット電力値（÷10 で W）

### 6. Influx line helpers（InfluxDB 行生成ヘルパー）
InfluxDB の行プロトコル形式でデータを作成：
- `appendInstantLine()`: 瞬時値（`switchbot_ble`）
- `appendHalfHourLine()`: 30 分集計（`switchbot_energy_30m`）
- `appendDayLine()`: 日次集計（`switchbot_energy_day`）

### 7. Energy integration（エネルギー積分ロジック）
- `integrateUntil()`: 前回のサンプル時刻から現在時刻までの電力を積分
  - 前回の測定電力を一定値と仮定
  - JST の 00:00・00:30・01:00・... 境界で自動的に区切る
  - 30 分ごと・24:00 ごとに InfluxDB 行を生成
  - ギャップが 120 秒を超える場合は補間をスキップ

### 8. Persistent daily accumulator checkpoint（永続ストレージ）
- `loadPersistentState()`: 起動時に ESP32 の NVS から日次積算値を復元
- `savePersistentState()`: 30 分境界ごとに日次積算値を NVS に保存
- **復元戦略**: 
  - 当日の完了した 30 分窓は NVS から復元
  - 現在進行中の 30 分窓は復元されない（穴あきデータを示すフラグ `coverage_complete=false`）

### 9. Network / InfluxDB（ネットワーク・InfluxDB）
- `ensureWiFi()`: WiFi 接続を確認・再接続（タイムアウト 20 秒）
- `writeInflux()`: InfluxDB への HTTPS POST でバッチ書き込み
  - TLS 証明書検証は無効（`client.setInsecure()`）
  - 認可: `Authorization: Token <INFLUX_TOKEN>`
  - HTTP ステータス 204 で成功と判定

### 10. Display（ディスプレイ表示）
- `updateDisplay()`: M5Core2 の LCD に現在の計測値をリアルタイム表示
  - ゲートウェイ ID と "3 plugs"
  - 各デバイスのラベル、MAC 末尾 4 文字、現在の電力 [W]
  - 当日の累積エネルギー [Wh]、BLE RSSI [dBm]

### 11. Setup / loop（初期化とメインループ）
- `syncClock()`: NTP で ESP32 のクロックを JST で同期（最大 30 秒）
- `setup()`: 
  - M5Core2・シリアル・WiFi・BLE・NVS の初期化
  - NVS から日次集計を復元
  - BLE スキャンの開始
  - ディスプレイに初期画面を表示
- `loop()`:
  1. 4 秒間の BLE スキャンを実行
  2. 受信した BLE サンプルを処理・積分
  3. InfluxDB にバッチ送信
  4. 30 分ごとに NVS にチェックポイント保存
  5. ディスプレイ更新
  6. 次のサイクルまで待機（10 秒周期）

## ライセンス

本プロジェクトは個人利用目的の IoT 実験、改良、学習用途を想定しています。必要に応じて、個別のライセンス条件を追加してください。
