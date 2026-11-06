# HPM 开发者中心 - 第三方应用接入文档

欢迎接入 HPM 开发者中心！本系统采用标准的 `AppKey`（应用身份）+ `AccessToken`（用户授权）双重认证机制，确保用户数据安全。

## 🚀 1. 快速开始

### 1.1 创建应用获取凭证
1. 登录 HPM 开发者中心，在左侧菜单找到 **“应用程序”**。
2. 填写信息并创建应用，您将获得：
   * **App ID**：您的应用唯一标识。
   * **App Key**：您的应用密钥（**仅显示一次，切勿泄露到前端**）。

### 1.2 用户授权 (OAuth 2.0)
引导用户在浏览器中访问：
```
https://hpm.astercore.dpdns.org/oauth/authorize?app_id=您的AppID&redirect_uri=您的回调地址
```
用户点击“授权”后，页面会重定向到您的 `redirect_uri`，并在 URL 上附带一个临时 `code`（有效期 10 分钟）。

### 1.3 换取 Access Token
您的**后端服务器**需携带 `code`、`appId`、`appKey` 请求以下接口换取用户凭证：

**POST** `https://hpm.astercore.dpdns.org/api/public/oauth/token`
```json
{
  "code": "上一步获取的code",
  "appId": "您的AppID",
  "appKey": "您的AppKey"
}
```
**响应示例**：
```json
{
  "success": true,
  "accessToken": "f47ac10b58cc4372...", 
  "expiresIn": 86400
}
```
*(注：`accessToken` 有效期为 1 天。建议您在数据库中建立您自己的用户与 HPM 用户的映射关系，并将此 Token 安全存储)*

---

## 🛠️ 2. 核心 API 调用

所有 API 请求必须使用 `POST` 方法，并遵循以下标准：

*   **接口地址**：`https://hpm.astercore.dpdns.org/api/webhooks/apps`
*   **请求头 (Headers)**：
    ```http
    Authorization: Bearer <您的 App Key>
    Content-Type: application/json
    ```
*   **请求体基础参数 (Body)**：
    *   `accessToken`：换取的用户凭证。
    *   `action`：执行的动作。

📖 2.1 读取用户信息

获取已授权用户的基本信息（包含用户名、邮箱、绑定的 GitHub 信息以及公钥）。

请求体：
```json

{
  "accessToken": "f47ac10b58cc4372...",
  "action": "get_user"
}
```
响应示例：
```json
{
  "name": "张三",
  "email": "zhangsan@example.com",
  "githubUrl": "https://github.com/zhangsan",
  "pgpKey": "-----BEGIN PGP PUBLIC KEY BLOCK-----\n...",
  "sshKey": "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI...",
  "repoUrl": "https://example.com/hpm"
}
```
githubUrl：如果用户绑定了 GitHub，则返回其 GitHub 主页地址；未绑定则返回 null。

pgpKey：如果用户填写了 PGP 公钥，则返回公钥内容；未填写则返回 null。

sshKey：如果用户填写了 SSH 公钥，则返回公钥内容；未填写则返回 null。

repoUrl：用户绑定的私域 HPM 仓库地址。如果用户未绑定或未验证通过，则返回 null。

---

### 📦 2.2 读取包列表
获取用户私域仓库中的所有包及版本信息。

**请求体**：
```json
{
  "accessToken": "f47ac10b58cc4372...",
  "action": "get_packages"
}
```

**响应示例**：
```json
[
  {
    "id": "pkg_123",
    "name": "ch552_usb_cdc",
    "kind": "driver",
    "requiresJson": "{\"pin\":{\"type\":\"gpio_out\"}}",
    "versions": [
      {
        "version": "1.0.0",
        "url": "https://仓库地址/ch552_usb_cdc-1.0.0.tar.gz",
        "sigUrl": "https://仓库地址/ch552_usb_cdc-1.0.0.tar.gz.sig",
        "sha256": "703f56842fe7a6...",
        "size": 19973,
        "license": "MIT"
      }
    ]
  }
]
```

---

### 📝 2.3 写入/修改包 (读写)
**注意**：此操作属于高危操作，推荐仅对受信任的开发者应用开放。您的应用可以通过此接口，使用用户的 `accessToken` 替用户在其私域仓库中新建包或追加版本。

*（为确保数据签名的一致性，纯通过 API 添加的包，其开发者证书需要预先存在于用户的私域仓库中，否则下载端证书链校验会失败。建议第三方应用仅通过 API 触发配置更新，具体的 tar.gz 和 .sig 文件仍由用户或构建流水线上传。）*

**请求体**：
```json
{
  "accessToken": "f47ac10b58cc4372...",
  "action": "update_package",
  "payload": {
    "name": "新包名",
    "kind": "driver", 
    "requiresJson": "{}",
    "version": "1.0.0",
    "url": "https://...",
    "sigUrl": "https://...",
    "sha256": "xxxxx",
    "size": 1024,
    "license": "MIT"
  }
}
```

**响应示例**：
```json
{
  "success": true,
  "message": "Package updated successfully"
}
```

*(注：后端会校验用户的当前 `twoFactorEnabled` 状态，如果用户处于冻结状态，接口将直接返回 403 拒绝。)*

---

## ⚠️ 3. 安全与错误处理

| 状态码 | 含义 | 处理建议 |
| :--- | :--- | :--- |
| `401` | Token 或 AppKey 无效、已过期 | 检查 Header 的 AppKey；如 Token 过期，引导用户重新走一遍 OAuth 授权流程。 |
| `403` | 用户已取消授权、账户被冻结 | 清理您本地与该用户的绑定关系，提示用户去 HPM 后台确认状态。 |
| `400` | 参数缺失或格式错误 | 检查 JSON 体是否符合文档要求。 |

*   **数据隔离**：使用 `accessToken` 请求时，系统底层会自动绑定该用户的数据，您无需且无法通过该 Token 访问其他用户的数据。
*   **写操作权限控制**：读取操作随时可用；写入操作受用户 `accessToken` 的权限范围保护。
