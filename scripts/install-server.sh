#!/usr/bin/env bash
# Start one private household backend without editing source code.
set -euo pipefail
passport_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
passport_server_dir="$passport_project_root/server"
passport_port="${PODCAST_PORT:-8899}"
passport_public_url="${PODCAST_PUBLIC_URL:-}"
passport_no_open=0
passport_port_explicit=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) [[ $# -ge 2 ]] || { echo '请给 --port 指定端口。' >&2; exit 2; }; passport_port="$2"; passport_port_explicit=1; shift 2;;
    --public-url) [[ $# -ge 2 ]] || { echo '请给 --public-url 指定硬件可访问的后台地址。' >&2; exit 2; }; passport_public_url="$2"; shift 2;;
    --no-open) passport_no_open=1; shift;;
    *) echo "未知选项：$1" >&2; exit 2;;
  esac
done
passport_valid_port() {
  [[ "$1" =~ ^[1-9][0-9]{0,4}$ && "$1" -le 65535 ]]
}
passport_valid_identity() {
  [[ "$1" =~ ^(0|[1-9][0-9]{0,9})$ && "$1" -le 4294967294 ]]
}
passport_valid_url() {
  [[ -z "$1" ]] && return 0
  [[ "$1" =~ ^https?://([A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?)(:([1-9][0-9]{0,4}))?/?$ ]] || return 1
  [[ -z "${BASH_REMATCH[4]}" ]] || passport_valid_port "${BASH_REMATCH[4]}"
}
passport_valid_port "$passport_port" || { echo '端口需在 1–65535 之间，且不要在前面加零。' >&2; exit 2; }
# Pick a reachable LAN address without changing network settings.
if [[ -z "$passport_public_url" && ! -f "$passport_server_dir/.env" ]]; then
  passport_lan_ip=""
  if command -v ip >/dev/null 2>&1; then
    passport_lan_ip="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++)if($i=="src"){print $(i+1);exit}}')"
  elif command -v ipconfig >/dev/null 2>&1 && command -v ifconfig >/dev/null 2>&1; then
    for passport_interface in $(ifconfig -l); do
      if [[ "$passport_interface" =~ ^en[0-9]+$ ]]; then
        passport_lan_ip="$(ipconfig getifaddr "$passport_interface" 2>/dev/null || true)"
        [[ -z "$passport_lan_ip" ]] || break
      fi
    done
  fi
  if [[ "$passport_lan_ip" =~ ^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)[0-9.]+$ ]]; then
    passport_public_url="http://$passport_lan_ip:$passport_port"
  fi
fi
if [[ -n "$passport_public_url" ]]; then
  passport_valid_url "$passport_public_url" || { echo '后台地址应为 http://服务器地址:端口 或 https://域名，不要带口令、查询或页面路径。' >&2; exit 2; }
fi
command -v docker >/dev/null 2>&1 || { echo '请先安装并启动 Docker（容器运行工具），再运行本安装脚本。' >&2; exit 1; }
docker compose version >/dev/null 2>&1 || { echo '需要 Docker Compose（组合启动工具）第二版。' >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo 'Docker 尚未启动，或当前用户无权使用。启动后重试。' >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo '请先安装 curl（网页连接检查工具），安装脚本需要它确认后台已就绪。' >&2; exit 1; }
[[ -f "$passport_server_dir/compose.yaml" ]] || { echo '缺少 server/compose.yaml，请下载完整项目。' >&2; exit 1; }
command -v id >/dev/null 2>&1 || { echo '缺少 id（运行身份检查工具），未更改安装配置。' >&2; exit 1; }
passport_current_uid="$(id -u)"
passport_current_gid="$(id -g)"
passport_valid_identity "$passport_current_uid" && passport_valid_identity "$passport_current_gid" || { echo '无法核验当前运行身份，未更改安装配置。' >&2; exit 1; }
umask 077
passport_env="$passport_server_dir/.env"
if [[ ! -f "$passport_env" ]]; then
  command -v openssl >/dev/null 2>&1 || { echo '首次安装需要 openssl（随机安装码生成工具）。' >&2; exit 1; }
  passport_setup_key="$(openssl rand -hex 32)"
  [[ "$passport_setup_key" =~ ^[0-9a-f]{64}$ ]] || { echo '生成安装码失败。' >&2; exit 1; }
  # Never overwrite an existing installation key, data directory or port.
  (set -o noclobber; printf 'PODCAST_SETUP_KEY=%s\nPODCAST_PORT=%s\nPODCAST_PUBLIC_URL=%s\nPODCAST_UID=%s\nPODCAST_GID=%s\n' "$passport_setup_key" "$passport_port" "$passport_public_url" "$passport_current_uid" "$passport_current_gid" > "$passport_env")
fi
chmod 600 "$passport_env"
for passport_field in PODCAST_SETUP_KEY PODCAST_PORT PODCAST_PUBLIC_URL PODCAST_UID PODCAST_GID; do
  [[ "$(awk -v key="$passport_field" 'index($0,key "=")==1 {count++} END{print count+0}' "$passport_env")" == 1 ]] || { echo '现有 .env 的必要设置缺失或重复；为保护原配置，未覆盖它。请参照安装说明检查。' >&2; exit 1; }
done
passport_saved_uid="$(sed -n 's/^PODCAST_UID=//p' "$passport_env")"
passport_saved_gid="$(sed -n 's/^PODCAST_GID=//p' "$passport_env")"
passport_valid_identity "$passport_saved_uid" && passport_valid_identity "$passport_saved_gid" || { echo '现有 .env 的运行身份无效；为保护原配置和资料，未覆盖它。' >&2; exit 1; }
[[ "$passport_saved_uid" == "$passport_current_uid" && "$passport_saved_gid" == "$passport_current_gid" ]] || { echo '这份安装属于另一运行身份，请使用原来安装后台的系统用户启动。配置和资料未覆盖，也未更改目录归属。' >&2; exit 1; }
passport_setup_key="$(sed -n 's/^PODCAST_SETUP_KEY=//p' "$passport_env")"
passport_saved_port="$(sed -n 's/^PODCAST_PORT=//p' "$passport_env")"
[[ "$passport_setup_key" =~ ^[0-9a-f]{64}$ ]] || { echo '现有 .env 安装码无效；为保护原配置，未覆盖它。请参照安装说明检查。' >&2; exit 1; }
passport_valid_port "$passport_saved_port" || { echo '现有 .env 的端口无效；为保护原配置，未覆盖它。' >&2; exit 1; }
if [[ -n "$passport_saved_port" && "$passport_port_explicit" == 0 ]]; then passport_port="$passport_saved_port"; fi
[[ -z "$passport_saved_port" || "$passport_saved_port" == "$passport_port" ]] || { echo '现有安装使用不同端口，请沿用原端口；脚本不会覆盖已有设置。' >&2; exit 1; }
passport_saved_public_url="$(sed -n 's/^PODCAST_PUBLIC_URL=//p' "$passport_env")"
passport_valid_url "$passport_saved_public_url" || { echo '现有 .env 的后台地址无效；为保护原配置，未覆盖它。' >&2; exit 1; }
if [[ -n "$passport_public_url" && "$passport_public_url" != "$passport_saved_public_url" ]]; then
  passport_env_pending="$(mktemp "$passport_server_dir/.env.pending.XXXXXX")"
  awk -v address="$passport_public_url" 'BEGIN{found=0} /^PODCAST_PUBLIC_URL=/{print "PODCAST_PUBLIC_URL=" address;found=1;next} {print} END{if(!found)print "PODCAST_PUBLIC_URL=" address}' "$passport_env" > "$passport_env_pending"
  chmod 600 "$passport_env_pending"; mv "$passport_env_pending" "$passport_env"
fi
if [[ -z "$passport_public_url" ]]; then passport_public_url="$passport_saved_public_url"; fi
mkdir -p "$passport_server_dir/data" "$passport_server_dir/media"
# Restrict plaintext credentials and listening records to the server owner.
chmod 700 "$passport_server_dir/data"
export PODCAST_PORT="$passport_port"
export PODCAST_SETUP_KEY="$passport_setup_key"
export PODCAST_PUBLIC_URL="$passport_public_url"
export PODCAST_UID="$passport_saved_uid"
export PODCAST_GID="$passport_saved_gid"
docker compose --env-file "$passport_env" -f "$passport_server_dir/compose.yaml" config --quiet
docker compose --env-file "$passport_env" -f "$passport_server_dir/compose.yaml" up -d --build
passport_local_url="http://127.0.0.1:$passport_port"
passport_ready=0
for ((passport_attempt=0;passport_attempt<60;passport_attempt++)); do
  if passport_health="$(curl -fsS --max-time 2 "$passport_local_url/healthz" 2>/dev/null)" && [[ "$passport_health" =~ \"ok\"[[:space:]]*:[[:space:]]*true ]]; then passport_ready=1; break; fi
  sleep 1
done
[[ "$passport_ready" == 1 ]] || { echo '后台尚未就绪，原数据仍保留。请查看 Docker 的后台日志后重试。' >&2; exit 1; }
passport_private_dir="$passport_project_root/.local"
mkdir -p "$passport_private_dir"; chmod 700 "$passport_private_dir"
passport_browser_url="${passport_public_url:-$passport_local_url}"
passport_install_url="$passport_browser_url/setup#install=$passport_setup_key"
# This private local page is ignored by Git and never served by the backend.
cat > "$passport_private_dir/first-setup.html" <<HTML
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>打开播客后台</title><body><h1>后台已启动</h1><p>在这台电脑上打开首次设置页面，创建自己的管理口令。</p><p><a href="$passport_install_url">打开首次设置</a></p><p>后台普通地址：$passport_browser_url 。硬件请使用同一网络中服务器的地址，不能填127.0.0.1。</p><p>这份本地说明包含一次性安装信息，不应发给别人或上传。</p></body></html>
HTML
chmod 600 "$passport_private_dir/first-setup.html"
printf '后台已启动：%s\n' "$passport_local_url"
printf '首次设置入口保存在 %s\n' "$passport_private_dir/first-setup.html"
echo '创建口令后，打开「我的设备」生成配对码，再用手机连接硬件的设置热点。'
if [[ "$passport_no_open" == 0 ]]; then
  if command -v open >/dev/null 2>&1; then open "$passport_private_dir/first-setup.html";
  elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$passport_private_dir/first-setup.html" >/dev/null 2>&1 || true;
  fi
fi
