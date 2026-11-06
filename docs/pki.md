# HPM 仓库 PKI

HPM 使用 X.509 证书链，不再对 `*.pub` 公钥文件做 detached signature。

```text
Root CA（离线根密钥）
  └── official.crt（官方仓库 CA）
        └── domain.crt（域仓库 CA）
              └── developer.crt（开发者）
```

- 根证书 PEM 固定在 `core/include/hpm/config.h` 的 `HPM_ROOT_CA_PEM`；
- 根私钥只保存在离线环境，不进入仓库，也不由 hpm 生成；
- 官方证书从 `HPM_OFFICIAL_CERT_URL` 下载；
- 域证书固定放在域仓库根目录 `official.crt`，主索引的 `domains.<name>.cert` 指向它；
- 开发者证书 URL 在域索引的 `developers.<name>.cert`；
- 索引和包仍然使用 detached signature，只是验签公钥来自证书。

---

## 1. 根证书（离线操作）

以下命令都使用根私钥，必须由持有根密钥的人执行，hpm 不执行这些操作。

```bash
openssl genpkey -algorithm ED25519 -out root.key

openssl req -x509 -new -key root.key -out root.crt -days 7300 \
  -subj "/CN=HPM Root CA/O=HPM" \
  -addext "basicConstraints=critical,CA:TRUE,pathlen:2" \
  -addext "keyUsage=critical,keyCertSign,cRLSign"
```

把 `root.crt` 的 PEM 内容替换到：

```c
// core/include/hpm/config.h
#define HPM_ROOT_CA_PEM R"PEM(-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)PEM"
```

`root.crt` 可以公开，`root.key` 必须离线保存。

---

## 2. 官方仓库证书

官方仓库私钥只保存在官方发布环境。

```bash
openssl genpkey -algorithm ED25519 -out official.key
openssl req -new -key official.key -out official.csr \
  -subj "/CN=HPM Official Registry/O=HPM"

cat > official.ext <<'EXTEOF'
basicConstraints=critical,CA:TRUE,pathlen:1
keyUsage=critical,keyCertSign,digitalSignature
extendedKeyUsage=codeSigning
subjectAltName=URI:hpm://official
EXTEOF

openssl x509 -req -in official.csr \
  -CA root.crt -CAkey root.key -CAcreateserial \
  -out official.crt -days 1825 -extfile official.ext
```

发布：

```text
official.crt  ->  HPM_OFFICIAL_CERT_URL
```

`official.key` 用于签主 `index.json`。

---

## 3. 域仓库证书

由官方仓库私钥签发。

```bash
openssl genpkey -algorithm ED25519 -out lab.key
openssl req -new -key lab.key -out lab.csr \
  -subj "/CN=HPM Domain lab/O=HPM"

cat > lab.ext <<'EXTEOF'
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,digitalSignature
extendedKeyUsage=codeSigning
subjectAltName=URI:hpm://domain/lab
EXTEOF

openssl x509 -req -in lab.csr \
  -CA official.crt -CAkey official.key -CAcreateserial \
  -out official.crt -days 365 -extfile lab.ext
```

主索引：

```json
{
  "schema": 2,
  "official_cert": "{baseurl}/official.crt",
  "domains": {
    "lab": {
      "index": "{baseurl}/lab/index.json",
      "cert": "https://example.com/lab-repo/official.crt"
    }
  },
  "developers": {},
  "packages": {}
}
```

`lab.key` 用于签域 `index.json`。域证书统一发布为域仓库根目录的：

```text
https://example.com/lab-repo/official.crt
```

---

## 4. 开发者证书

由域仓库私钥签发。

```bash
openssl genpkey -algorithm ED25519 -out alice.key
openssl req -new -key alice.key -out alice.csr \
  -subj "/CN=HPM Developer alice/O=HPM"

cat > alice.ext <<'EXTEOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=codeSigning
subjectAltName=URI:hpm://developer/alice
EXTEOF

openssl x509 -req -in alice.csr \
  -CA official.crt -CAkey lab.key -CAcreateserial \
  -out alice.crt -days 365 -extfile alice.ext
```

域索引：

```json
{
  "schema": 2,
  "official_cert": "{baseurl}/lab/lab.crt",
  "developers": {
    "alice": {
      "cert": "{baseurl}/alice.crt"
    }
  },
  "packages": {}
}
```

`alice.key` 只用于签自己的包，不再签索引。

---

## 5. 索引和包签名

主索引：

```bash
openssl pkeyutl -sign -inkey official.key -rawin \
  -in index.json -out index.json.sig
```

域索引：

```bash
openssl pkeyutl -sign -inkey lab.key -rawin \
  -in lab/index.json -out lab/index.json.sig
```

包：

```bash
openssl pkeyutl -sign -inkey alice.key -rawin \
  -in alice/pkg.tar.gz -out alice/pkg.tar.gz.sig
```

RSA/ECDSA 私钥使用：

```bash
openssl dgst -sha256 -sign key.pem -out data.sig data
```

---

## 6. hpm-dev publish

`hpm-dev publish` 现在只：

1. 打包；
2. 用开发者私钥签 `package.tar.gz`；
3. 把 tar + sig 放进仓库；
4. 同步开发者中心。

它**不更新 `index.json`，也不签 `index.json`**。索引由官方/域仓库的 CI 或管理员用对应私钥更新和签名。

---

## 7. 测试用覆盖

```bash
export HPM_ROOT_CA=/path/to/root.crt
export HPM_OFFICIAL_CERT=file:///path/to/official.crt
export HPM_REGISTRY=file:///path/to/index.json
```

生产环境不要设置这些变量。
