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

## ライセンス

本プロジェクトは個人利用目的の IoT 実験、改良、学習用途を想定しています。必要に応じて、個別のライセンス条件を追加してください。
