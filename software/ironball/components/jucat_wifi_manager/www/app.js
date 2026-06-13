/**
 * IronBall WiFi Config — Frontend Logic
 *
 * API endpoints:
 *   GET  /api/wifi-status   → {connected, ssid, ip, rssi, status}
 *   GET  /api/wifi-scan     → [{ssid, rssi, auth}, ...]
 *   POST /api/wifi-config   → {ssid, password} → {status, message}
 */

// ===== Helpers =====
function $(id) { return document.getElementById(id); }

function showMessage(text, type) {
    const el = $('message');
    el.textContent = text;
    el.className = type; // "success" | "error" | "info"
}

function hideMessage() {
    $('message').className = 'hidden';
}

async function apiFetch(url, opts) {
    const res = await fetch(url, opts);
    if (!res.ok) throw new Error('HTTP ' + res.status);
    return res.json();
}

function setBtnLoading(btnId, loading) {
    const btn = $(btnId);
    btn.disabled = loading;
    btn.innerHTML = loading
        ? '<span class="spinner"></span> 处理中...'
        : btn.dataset.origText || btn.textContent;
}

// ===== Status Polling =====
async function loadStatus() {
    try {
        const data = await apiFetch('/api/wifi-status');
        $('status-text').textContent = data.status || 'unknown';
        $('status-ssid').textContent = data.ssid || '--';
        $('status-ip').textContent = data.ip || '--';
        $('status-rssi').textContent = data.rssi != null ? data.rssi + ' dBm' : '--';
    } catch (err) {
        $('status-text').textContent = '获取失败';
        console.warn('Status poll error:', err);
    }
}

// ===== WiFi Scan =====
async function scanWifi() {
    const btn = $('scan-btn');
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner"></span> 扫描中...';

    hideMessage();

    try {
        const list = await apiFetch('/api/wifi-scan');
        const sel = $('ssid');
        sel.innerHTML = '<option value="">-- 选择网络 --</option>';

        // Sort by RSSI (strongest first)
        list.sort((a, b) => b.rssi - a.rssi);

        // Deduplicate by SSID
        const seen = new Set();
        let count = 0;

        for (const ap of list) {
            if (!ap.ssid || seen.has(ap.ssid)) continue;
            seen.add(ap.ssid);

            const opt = document.createElement('option');
            opt.value = ap.ssid;
            opt.textContent = ap.ssid + '  (' + ap.rssi + ' dBm)';
            sel.appendChild(opt);
            count++;
        }

        showMessage('已找到 ' + count + ' 个网络', 'info');
    } catch (err) {
        showMessage('扫描失败: ' + err.message, 'error');
    } finally {
        btn.disabled = false;
        btn.textContent = '🔄 扫描';
    }
}

// ===== Submit Config =====
async function submitConfig() {
    const ssid = $('ssid').value.trim();
    const password = $('password').value.trim();

    if (!ssid) {
        showMessage('请先选择或输入 SSID', 'error');
        return;
    }

    hideMessage();
    setBtnLoading('connect-btn', true);

    try {
        const data = await apiFetch('/api/wifi-config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ ssid, password })
        });

        if (data.status === 'ok') {
            showMessage('✅ 配置已保存，正在连接 ' + ssid + ' ...', 'success');
        } else {
            showMessage('❌ ' + (data.message || '配置失败'), 'error');
        }
    } catch (err) {
        showMessage('请求失败: ' + err.message, 'error');
    } finally {
        setBtnLoading('connect-btn', false);
    }
}

// ===== Init =====
document.addEventListener('DOMContentLoaded', function () {
    // Save original button text for loading state restore
    const connBtn = $('connect-btn');
    connBtn.dataset.origText = connBtn.textContent;

    // Load status and scan on page load
    loadStatus();
    scanWifi();

    // Auto-refresh status every 5s
    setInterval(loadStatus, 5000);
});
