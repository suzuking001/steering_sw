# Web APIと設定仕様

APIは同一機器の `/api/` 以下でJSONを使用します。設定用Wi-Fi・家庭や車内LANのどちらからも、ユーザーのログインやWebパスワードは不要です。

画面は最初に `GET /api/session` で自動生成された `csrf` トークンを取得し、更新要求には `X-CSRF-Token` ヘッダーを付けます。Cookieやユーザーセッションはありません。トークンは再起動で更新します。アクセス先のHostは、その接続先インターフェースのIPアドレスまたは `steering-sw.local` とします。これはブラウザーからの意図しない変更を防ぐための内部処理で、パスワード入力は必要ありません。

## エンドポイント

| メソッド | パス | 入力・動作 |
|---|---|---|
| GET | `/api/session` | CSRFトークン取得。ログイン不要 |
| GET | `/api/status` | 入力値、認識状態、学習状態、ログ、BLE・Wi-Fi状態。`?logs=0` で操作ログを省略 |
| POST | `/api/firmware/check` | GitHub Releasesの確認を開始。状態は `/api/status` の `firmware` で取得 |
| POST | `/api/firmware/install` | `{"version":"1.0.1"}`。確認済みの版を取得・検証し、再起動する処理を開始 |
| GET | `/api/config` | 現在の公開設定JSON |
| PUT | `/api/config` | 設定JSONを検証・保存・適用 |
| POST | `/api/validate` | 設定JSONを検証・正規化。保存しない |
| GET | `/api/defaults` | 現在のSSIDを維持した参考プリセット。保存しない |
| GET | `/api/backup` | 公開設定JSONをダウンロード |
| POST | `/api/restore` | `/api/config` の保存と同じ動作 |
| POST | `/api/learn/start` | `{"channel":0}`。系統0または1を測定 |
| POST | `/api/learn/stop` | 学習中止 |
| POST | `/api/test` | `{"action":"macro:maps"}` で保存済み操作をテスト。`{"action":"test_digits"}` で固定の `0123456789` を送信。登録したパスコードの送信は実ボタンの長押し専用 |
| PUT | `/api/ble/input-mode` | `{"enabled":true}` でキー送信を一時停止、falseで再開。設定は保存しない |
| POST | `/api/stop` | 実行中マクロを中止し、入力の再待機。送信の有効設定自体は維持 |
| PUT | `/api/passcode` | `{"passcode":"数字列"}`。空文字で削除 |
| PUT | `/api/wifi` | `{"ssid":"SSID","password":"Wi-Fiパスワード"}`。同じSSIDではpassword省略で登録済みの値を維持。空文字は明示的なパスワードなし、空SSIDは設定用APのみ |
| POST | `/api/wifi/reconnect` | 保存済みの接続先で再接続を開始 |
| POST | `/api/reset` | `{"confirm":"RESET"}`。入力・Wi-Fi・パスコードを初期化 |
| POST | `/api/ble/forget` | ペアリング情報を削除して再起動 |
| POST | `/api/ble/reconnect` | 現在の接続を切り、ペアリングを保持したまま接続を再受付 |
| POST | `/api/reboot` | 本体再起動 |

正常応答は200、形式・設定のエラーは400、Host・CSRFは403、操作不能は409、サイズ超過は413、保存失敗は507です。エラー応答は `{"error":"説明"}` です。

ファームウェア更新の開始応答は202です。状態には固定の更新元 `repository`、現在版 `version`、配布版 `availableVersion`、進捗 `received` と `size`（バイト）、`stage`、`message`、初回起動検証中の `bootChecking`、トークン登録状態の `tokenSet` を含みます。トークンの値はAPIに含めません。

更新の確認・実行中、`/api/status` は進捗とWi-Fi接続状態のみを返します。この間はBluetoothと各種設定の更新を一時停止します。

通常時の `/api/status` の `ble` に、設定名と有効状態、接続受付 `advertising`、暗号化 `encrypted`、`keyboardReady`、`mediaReady`、保存済みペアリング件数 `bonds` が含まれます。`bleReady` は暗号化済みの接続で少なくとも一方のキー通知を送れる状態です。操作の開始には、その操作で使うすべてのキー通知が必要です。

`protocolMode` は標準の `report` またはキーボード互換の `boot` です。`reportMapRead` は今回の接続で端末がレポートマップを読み取った場合に `true` です。端末が以前の接続で読み込んだ情報を利用する場合もあります。`disconnectReason` は起動後の最後のNimBLE切断コードで、記録がなければ `0` です。`advertisingIntervalMs` は接続待ち信号を出しているときの送信間隔です。`automaticReconnects` は起動後にキー通知待ちの接続を自動で切り直した回数です。

`serviceRefreshes` は起動後にサービス情報の再確認を通知した回数です。`automaticRetryUsed` は現在のキー通知待ちについて自動再試行を使ったことを示します。キー通知が可能になったときや手動で再接続したときに `false` に戻ります。

設定保存・復元APIからSSIDを変更することはできません。現在のSSIDを使用し、接続先は `/api/wifi` で変更します。WebのJSON読み込みは現在のSSIDを維持し、出力をOFFにした状態で検証してから編集画面へ反映します。確認して「保存・適用」を押すまでは保存されません。

## 設定JSON（version 1）

実際の完全な設定は `/api/config` かバックアップで取得してください。すべての必須フィールドを含むJSON全体を更新します。公開JSONには次を含みます。

- `version`: 1。
- `name`: 設定名。UTF-8で96バイトまで。
- `ssid`: 現在のWi-Fi SSID。32バイトまで。秘密情報は含まない。
- `outputEnabled`: 通常のキー送信を有効にする真偽値。初期値false。
- `ble`: 端末に表示する `name`（UTF-8で29バイトまで）と `enabled`。旧設定の読み込み時は初期値の `Steering Switch` と `true` を使用します。既存の本体へ旧バックアップを読み込む場合は現在のBluetooth設定を引き継ぎます。
- `channels`: 2要素。各要素は `name`, `mode` (`mv` / `adc` / `ohm`), `pullup` (Ω), `supplyMv` (mV), `idleCenter`, `idleTolerance`, `hysteresis`。
- `buttons`: 最大32要素、各系統16まで。各要素は `id`, `name`, `channel` (0 / 1), `center`, `tolerance`, `shortAction`, `doubleAction`, `longAction`。
- `macros`: 最大16要素。各要素は `id`, `name`, `steps`（1〜32要素）。標準の `maps`, `youtube`, `siri`, `lock`, `app_list` は残す必要がある。ステップ内容は編集可能。
- `timing`: `debounceMs`, `longMs`, `doubleMs`, `repeatDelayMs`, `repeatMs`, `wakeDelayMs`, `wakeSpace`。
- `acc`: `enabled`, `thresholdMv`, `debounceMs`。

数値と真偽値はJSONの数値型・真偽値型を使用します。数値を文字列にしないでください。IDは英数字・`_`・`-` の1〜24文字。設定JSONは最大24KBです。未知の秘密情報フィールドは読み込まず、正規化した公開フィールドだけを保存します。

判定は閉区間 `center ± tolerance` です。同じ系統のボタン同士、および未押下範囲との重複を拒否します。ヒステリシスは現在のボタンを未割り当ての隙間へ保持するために使用し、他の明示的なボタン／未押下範囲を優先します。Ω判定で測定不能の場合は操作を中止し、安定した未押下を再確認するまで入力を受け付けません。

## 操作とマクロ

操作IDは `none`, `volume_up`, `volume_down`, `play_pause`, `mute`, `next`, `previous`, `home`, `switch_app`, `maps`, `youtube`, `siri`, `lock`, `app_list`, `passcode`, `macro:登録ID`。

`maps`, `youtube`, `siri`, `lock`, `app_list` は同名の標準マクロを実行します。`passcode` は実ボタンの長押し専用です。音量を短押しへ割り当てる場合、ダブルクリックと長押しは `none` にします。

```json
{
  "id": "example",
  "name": "ショートカットと待機",
  "steps": [
    {"type": "chord", "key": 30, "modifiers": 7},
    {"type": "wait", "ms": 500},
    {"type": "media", "usage": 205},
    {"type": "text", "text": "sample"}
  ]
}
```

通常キーはUSB HID Usage ID（10進数4〜115）、修飾キーは8bitビットマスクです。左Control=1、Shift=2、Alt=4、GUI=8、右側はそれぞれ16、32、64、128。Webは左側4キーを編集します。

メディアUsage IDは音量＋=233、音量－=234、消音=226、再生／一時停止=205、次=181、前=182、停止=183。文字列は1〜128文字の印字可能ASCIIです。US配列のHIDキーへ変換するため、記号は接続先の配列・入力モードの影響を受けます。

1キーにつき30ms押下し、解放後に30ms空けます。待機は1ステップ10000ms以内、合計は概算30秒以内です。同時に実行できる操作は1つで、実行中に来た追加操作をキューに残しません。BLE通知失敗・切断時は中止し、再接続時に再送しません。

## 学習

開始後400msを準備時間とし、続く2秒間を5ms周期で測定します。100サンプル未満、測定不能サンプルの混入、大きな変動はエラーにします。中心は平均、許容幅は中心から最大偏差＋余裕です。電圧とADCの余裕は10、Ωの余裕は2Ωまたは中心値の3%の大きい方です。

このAPIの結果は `/api/status` の `learning` に出力します。Webの学習画面は `readings` の現在値を使います。判定単位が `mv` の系統では電圧を中心値に登録し、`adc` または `ohm` では対応する判定値を使います。学習画面を開いている間はキー送信を一時停止します。

「本体設定」の未押下の中心値も、対象系統の `readings` の現在値から取り込みます。「現在値を設定する」を押すと入力欄が更新され、「保存・適用」で本体へ反映されます。

## 設定用Wi-Fiへのかんたん接続

設定用APの起動時にワイルドカードDNSを開始し、端末のHTTP接続確認を `http://192.168.4.1/` へ302で案内します。設定用APに接続したクライアントのみが対象です。通常LAN側の要求と不明な `/api/` への要求は転送しません。自動表示は端末依存のため、IP直指定の方法を維持しています。

設定用APは電源投入時に起動し、家庭・車内のWi-Fiと並行して動作します。起動に失敗した場合は2秒間隔で再試行します。ESP32 DEVKIT V1のBOOT（GPIO0）を起動後に3秒長押しすると、すぐに再試行します。Wi-Fiパスワードは維持し、Web管理パスワードとログインは廃止しました。以前の管理パスワードは読み出したり使用したりしません。APIとログにWi-Fiパスワードを追加公開する変更はありません。

## 文字入力モード

`PUT /api/ble/input-mode` に `{"enabled":true}` を送ると、実行中の操作を中止し、Bluetooth接続を保ったままキー送信を一時停止します。`false` で一時停止を解除します。Web画面は `client` に16文字の識別子を添え、入力中は4秒ごとに一時停止を更新します。その要求は最後の更新から15秒で期限切れになります。識別子付きの再開要求は、直近の停止を要求した画面にだけ適用します。`/api/status` の `inputModePaused` がキー送信の一時停止を示します。

入力タスク・Web・学習は継続します。解除後は安定した未押下状態を確認してから、新しい操作を受け付けます。モードはRAM上のみで、再起動すると解除されます。

## Wi-Fiの保存・接続状態

Wi-Fi設定のPUTが成功した時点では「保存済み」であり、ルーターへの接続成功はまだ保証しません。`GET /api/status` には次を含みます。パスワード本文は含みません。

- `wifiConfigured`, `wifiSavedSsid`, `wifiPasswordSet`: 保存済みの接続先と、パスワード登録の有無。
- `wifiState`: `unconfigured`, `connecting`, `getting_ip`, `connected`, `failed`。
- `wifiConnected`, `wifiConnectedSsid`, `ip`, `wifiGateway`, `rssi`: 実際の接続状態。
- `wifiLanUrl`: 接続成功時にPCから使用するURL。未接続時は空文字。
- `wifiError`, `wifiFailureReason`, `wifiAttempts`: 接続に関する診断情報。
- `mdnsReady`: mDNSサービス開始の成否。PCで名前解決できることを保証する値ではない。

更新後は非同期の切断を開始し、300ms待ってから接続を開始します。1回の接続試行は30秒待ち、途中で15秒ごとの再接続を重ねません。自前のスケジューラーが再試行を管理し、切断理由の受け取りは別スレッドのWi-Fiイベントからatomic値を介して行います。DHCPホスト名は `steering-sw` とし、新しいIPで接続成功した際にmDNSを開始し直します。

Wi-Fi設定はSSIDを変更する場合にパスワード指定を必要とし、パスワードなしの場合は明示的に空文字を渡します。同じSSIDでパスワードを省略すると、保存済みの値を維持します。Webは更新要求を最大10秒、取得要求を4秒で中断し、通信が戻った際に再度状態を取得できます。403で変更が拒否された場合だけトークンを再取得し、変更を1度再試行します。タイムアウトした操作を自動で再送しません。

画面下の保存では、本体設定、端末パスコード、Wi-Fiの順に未保存の項目を保存します。各APIの保存は独立しており、途中で失敗した場合は残りの入力を維持します。秘密情報は保存後もそのページの入力値として残し、ページ再表示時には読み出さず登録状態を表示します。
