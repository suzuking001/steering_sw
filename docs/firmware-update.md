# GitHubからファームウェアを更新する

「本体設定 → ファームウェア更新」で、`suzuking001/steering_sw` のGitHub Releasesから新しいバージョンを取得します。更新元は初期状態からこのリポジトリに固定されています。公開リリースにはトークンの登録なしでアクセスでき、設定画面もファームウェアと一緒に更新されます。

## 初回のUSB準備

旧版のWeb画面で「保存済み設定をJSONでダウンロード」して控えます。USBでESP32をつなぎ、PlatformIOの **Upload**（`pio run -t upload`）で `1.0.1` 以降を書き込みます。従来の構成からは全フラッシュ消去を行わずに移行できます。ボタン設定を保存する `settings` と、Wi-Fi・端末パスコード・ペアリング情報を保存するNVSは従来の位置に残します。

## Web画面で更新する

1. ESP32をインターネットに接続できる2.4GHzのWi-Fiにつなぎます。
2. 「本体設定 → ファームウェア更新」で **更新を確認** を押します。
3. 新しいバージョンが表示されたら **更新して再起動** を押します。完了後、現在のバージョンを確認してページを再読み込みします。

更新は停車中に行い、完了するまで電源を維持してください。ESP32は別の領域へ書き込み、ファイルのサイズ・SHA-256・ESP32アプリ形式を検証してから再起動します。新しいアプリの初回起動で設定用Wi-Fiと入力読み取りが立ち上がれば、正常起動として確定します。取得や検証に失敗した場合はそのまま使用でき、起動確認に失敗した場合は前の版に戻ります。USBのUploadは復旧にも使用できます。

## 更新元を非公開で運用する場合

GitHubの非公開リポジトリでは、リリースの取得にも認証が必要です。GitHubの **Fine-grained personal access token** を作成し、Resource ownerを `suzuking001`、Repository accessを **Only select repositories** の `steering_sw`、Repository permissionsの **Contents** を **Read-only** にします。 [GitHubの作成手順](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/managing-your-personal-access-tokens#creating-a-fine-grained-personal-access-token)

PlatformIOのシリアルモニターを115200bpsで開き、`GITHUB_TOKEN ` に続けてトークンを送ります。`GitHub token registered.` が出れば登録完了です。トークンはESP32のNVSに保存されます。期限切れのトークンは同じ方法で入れ替えられ、`GITHUB_TOKEN_CLEAR` で削除できます。

## 新しい版を配布する

`VERSION` を `1.0.2` のように増やしてコミットし、そのコミットに対応する `v1.0.2` のタグをGitHubへプッシュします。GitHub ActionsがESP32向けにビルドし、同じタグのリリースに `steering-sw-esp32dev.bin` を添付します。ESP32にUSBで入れた版より新しい正式リリースが更新候補になります。配布ファイルの上限は1.5 MiBです。
