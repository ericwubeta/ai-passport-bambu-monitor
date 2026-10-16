# tools/get-token.ps1 —— 拓竹 accessToken / User ID / 打印机序列号 一键获取
# 零依赖:只用 Windows 自带 PowerShell,不用装 Python。
# 用法:双击同目录 get-token.bat(自动绕过脚本执行策略),按提示操作。
# 隐私:token 只存本机(%USERPROFILE%\.bambu_provision.json),不会上传到任何地方。

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = `
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$BAMBU = "https://api.bambulab.cn/v1"

# 在嵌套 JSON 里递归找指定键的标量值(uid 有时是数字有时是字符串,都收)
function Find-Value($obj, $key) {
    if ($null -eq $obj -or $obj -is [string]) { return $null }
    if ($obj -is [System.Collections.IEnumerable]) {
        foreach ($item in $obj) {
            $r = Find-Value $item $key
            if ($null -ne $r) { return $r }
        }
        return $null
    }
    if ($obj -is [System.Management.Automation.PSCustomObject]) {
        foreach ($p in $obj.PSObject.Properties) {
            $v = $p.Value
            if ($p.Name -eq $key -and $null -ne $v -and
                ($v -is [string] -or $v -is [int] -or $v -is [long] -or $v -is [decimal] -or $v -is [double])) {
                if ("$v" -and "$v" -ne "0") { return "$v" }
            }
            $r = Find-Value $v $key
            if ($null -ne $r) { return $r }
        }
    }
    return $null
}

Write-Host "=== 拓竹 Token 获取工具 (中国区) ===" -ForegroundColor Green
Write-Host ""

$phone = Read-Host "拓竹账号手机号"
if ($phone -notmatch '^\d{6,15}$') {
    Write-Host "手机号格式不对" -ForegroundColor Red
    exit 1
}

try {
    Invoke-RestMethod -Method Post -Uri "$BAMBU/user-service/user/sendsmscode" `
        -ContentType "application/json; charset=utf-8" `
        -Body (@{ phone = $phone; type = "codeLogin" } | ConvertTo-Json) | Out-Null
    Write-Host "验证码已发送到手机(1~2 分钟内有效)" -ForegroundColor Cyan
} catch {
    Write-Host "发送失败: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$code = Read-Host "短信验证码(6 位数字)"
if ($code -notmatch '^\d{6}$') {
    Write-Host "验证码应为 6 位数字" -ForegroundColor Red
    exit 1
}

try {
    $login = Invoke-RestMethod -Method Post -Uri "$BAMBU/user-service/user/login" `
        -ContentType "application/json; charset=utf-8" `
        -Body (@{ account = $phone; code = $code } | ConvertTo-Json)
} catch {
    Write-Host "登录失败: $($_.Exception.Message) (验证码可能已过期,重跑一次)" -ForegroundColor Red
    exit 1
}

$token = Find-Value $login "accessToken"
if (-not $token) {
    Write-Host "登录响应里没有 accessToken(验证码可能已过期,重跑一次)" -ForegroundColor Red
    exit 1
}

# User ID 优先从登录响应取,取不到再查 preferences 接口
$uid = Find-Value $login "uid"
if (-not $uid) {
    try {
        $pref = Invoke-RestMethod -Method Get -Uri "$BAMBU/design-user-service/my/preference" `
            -Headers @{ Authorization = "Bearer $token" }
        $uid = Find-Value $pref "uid"
    } catch { }
}

# 账号下打印机列表(SN + 名字)
$snList = @()
try {
    $bind = Invoke-RestMethod -Method Get -Uri "$BAMBU/iot-service/api/user/bind" `
        -Headers @{ Authorization = "Bearer $token" }
    foreach ($d in $bind.devices) {
        if ($d.dev_id) { $snList += "$($d.dev_id)  $($d.name)" }
    }
} catch { }

Write-Host ""
Write-Host "=== 登录成功 ===" -ForegroundColor Green
Write-Host "User ID : $(if ($uid) { $uid } else { '(未取到,可稍后在设备页查看)' })"
Write-Host "打印机列表(填进配网页的序列号就是它):"
if ($snList.Count -eq 0) {
    Write-Host "  (账号下没有打印机)"
} else {
    foreach ($s in $snList) { Write-Host "  $s" }
}
Write-Host ""
Write-Host "accessToken(粘贴进设备配网页):" -ForegroundColor Cyan
Write-Host $token -ForegroundColor Yellow
try {
    Set-Clipboard $token
    Write-Host "(已自动复制到剪贴板,直接去配网页 Ctrl+V)" -ForegroundColor DarkGray
} catch { }

$out = Join-Path $env:USERPROFILE ".bambu_provision.json"
@{ region = "china"; token = $token; uid = "$uid"; devices = $snList } |
    ConvertTo-Json | Set-Content -Path $out -Encoding UTF8
Write-Host "已保存到 $out"
Write-Host ""
Write-Host "下一步:手机连设备热点 Bambu-Setup(密码 setup123)" -ForegroundColor Cyan
Write-Host "       浏览器打开 http://192.168.4.1 填 WiFi + User ID + Token + 序列号"
