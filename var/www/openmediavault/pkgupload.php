<?php

/**
 * This file is part of openmediavault-pluginmgr.
 *
 * @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    ${GITHUB_USER}
 *
 * OpenMediaVault is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * OpenMediaVault is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/**
 * Self-service upload endpoint for .deb packages.
 *
 * The declarative OpenMediaVault workbench does not provide any file
 * upload facility, thus this endpoint complements the 'PkgMgr' RPC
 * service: it accepts a multipart/form-data POST from the administrator's
 * browser and stores the uploaded Debian binary package in /tmp. From
 * there it can be picked and installed on the 'Local deb' page of the
 * package manager plugin (System -> Package Manager).
 *
 * The OpenMediaVault session is a plain PHP session with a Strict
 * SameSite cookie, so a same-origin browser request automatically
 * carries the credentials of the logged in web interface user. As an
 * additional line of defence the Origin/Sec-Fetch-Site headers are
 * validated when present.
 *
 * NOTE: Uploaded files intentionally stay in /tmp. They are cleaned up
 * by the regular housekeeping of the temporary directory, no manual
 * intervention is required (or performed).
 */

// Deny direct access to this file outside of a PHP processing context.
if (!isset($_SERVER['REQUEST_METHOD'])) {
    exit;
}

require_once("openmediavault/autoloader.inc");
require_once("openmediavault/env.inc");
require_once("openmediavault/functions.inc");

/**
 * Send a JSON RPC-style envelope and terminate.
 * @param response The response payload (or NULL).
 * @param httpStatus The HTTP status code.
 * @param message An error message (or NULL).
 */
function sendJson($response, $httpStatus = 200, $message = null)
{
    header("Content-Type: application/json");
    header("Cache-Control: max-age=0, no-cache, no-store, must-revalidate");
    http_response_code($httpStatus);
    print json_encode_safe([
        "response" => $response,
        "error" => (null === $message) ? null : [
            "code" => $httpStatus,
            "message" => $message
        ]
    ]);
    exit;
}

/**
 * Send a plain text error page and terminate.
 * @param httpStatus The HTTP status code.
 * @param messageEn The English error message.
 * @param messageZh The Chinese error message.
 */
function sendError($httpStatus, $messageEn, $messageZh)
{
    header("Content-Type: text/plain; charset=UTF-8");
    http_response_code($httpStatus);
    print (0 === strpos($_SERVER['HTTP_ACCEPT_LANGUAGE'] ?? "", "zh"))
        ? $messageZh : $messageEn;
    exit;
}

/**
 * Start the session and make sure an authenticated administrator is
 * calling this endpoint.
 * @return The session instance.
 */
function assertAdministratorSession()
{
    $session = &\OMV\Session::getInstance();
    $session->start();
    // Defence in depth against cross-site request forgery: the session
    // cookie is SameSite=Strict, but reject suspicious cross-origin
    // requests even before touching the session.
    if (isset($_SERVER['HTTP_SEC_FETCH_SITE']) &&
        !in_array($_SERVER['HTTP_SEC_FETCH_SITE'], ["same-origin", "none"])) {
        sendError(403, "Cross-site requests are not allowed.",
            "不允许跨站请求。");
    }
    if (isset($_SERVER['HTTP_ORIGIN'])) {
        $expected = ((array_key_exists("HTTPS", $_SERVER) &&
            !empty($_SERVER['HTTPS'])) ? "https://" : "http://").
            $_SERVER['HTTP_HOST'];
        if (0 !== strpos($_SERVER['HTTP_ORIGIN'], $expected)) {
            sendError(403, "Cross-site requests are not allowed.",
                "不允许跨站请求。");
        }
    }
    if (!$session->isAuthenticated()) {
        sendError(401, "Not logged in. Please log in to the ".
            "OpenMediaVault web interface first, then reopen this page.",
            "未登录。请先登录 OpenMediaVault 网页界面，然后重新打开本页。");
    }
    if (OMV_ROLE_ADMINISTRATOR !== $session->getRole()) {
        sendError(403, "Administrator privileges are required.",
            "需要管理员权限。");
    }
}

/**
 * Validate the name of the uploaded file. Only plain file names with
 * safe characters and the '.deb' extension are accepted.
 * @param filename The original name of the uploaded file.
 * @return The sanitized file name.
 */
function sanitizeDebFilename($filename)
{
    $filename = basename(str_replace("\\", "/", $filename));
    if ((1 !== preg_match("/^[A-Za-z0-9._+-]+\.deb$/", $filename)) ||
        (false !== strpos($filename, ".."))) {
        sendJson(null, 400, sprintf(
            "The file name '%s' is invalid. Only plain .deb file names ".
            "with the characters A-Z a-z 0-9 . _ + - are accepted.",
            $filename
        ));
    }
    return $filename;
}

/**
 * Move the uploaded file to /tmp and read its package meta data.
 * @param filename The sanitized file name.
 * @return An associative array describing the uploaded package.
 */
function storeUploadedDeb($filename)
{
    $target = DIRECTORY_SEPARATOR."tmp".DIRECTORY_SEPARATOR.$filename;
    // Check whether the upload succeeded.
    if (!isset($_FILES['file']) || !is_array($_FILES['file'])) {
        sendJson(null, 400, "No file has been uploaded.");
    }
    switch ($_FILES['file']['error']) {
        case UPLOAD_ERR_OK:
            break;
        case UPLOAD_ERR_INI_SIZE:
        case UPLOAD_ERR_FORM_SIZE:
            sendJson(null, 413, "The uploaded file is too large. The ".
                "limit is 25 MiB (nginx/PHP configuration of the web ".
                "interface).");
        default:
            sendJson(null, 400, "The file upload failed ".
                "(error code ".intval($_FILES['file']['error']).").");
    }
    if (!is_uploaded_file($_FILES['file']['tmp_name'])) {
        sendJson(null, 400, "Invalid upload request.");
    }
    // Verify the file really is a Debian binary package.
    $finfo = new \finfo(FILEINFO_NONE);
    if (1 !== preg_match("/^Debian binary package/",
        strval($finfo->file($_FILES['file']['tmp_name'])))) {
        sendJson(null, 400, "The uploaded file '".$filename.
            "' is not a Debian binary package.");
    }
    // Move the file to /tmp. An already existing file with the same
    // name that is owned by this process is replaced. If the existing
    // file cannot be replaced (e.g. the sticky bit of /tmp prevents
    // removing a file owned by another user, such as a file that has
    // been copied there via scp), store the upload under a numbered
    // file name instead.
    if (file_exists($target)) {
        if (!@unlink($target) || file_exists($target)) {
            $base = substr($filename, 0, -4);
            $i = 1;
            do {
                $target = DIRECTORY_SEPARATOR."tmp".DIRECTORY_SEPARATOR.
                    $base."-".strval($i).".deb";
                $i++;
            } while (file_exists($target));
        }
    }
    if (!move_uploaded_file($_FILES['file']['tmp_name'], $target)) {
        sendJson(null, 500, "Failed to store the uploaded file in /tmp.");
    }
    chmod($target, 0644);
    // Read the package meta data (best effort, non-fatal).
    $info = [
        "filename" => basename($target),
        "size" => filesize($target),
        "package" => "",
        "version" => "",
        "architecture" => ""
    ];
    try {
        $cmd = new \OMV\System\Process("dpkg-deb", [
            "--field", escapeshellarg($target),
            "Package", "Version", "Architecture"
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 === $exitStatus) {
            foreach ($output as $line) {
                if (1 === preg_match("/^(\S+):\s*(.*)$/", $line, $m)) {
                    $info[strtolower($m[1])] = trim($m[2]);
                }
            }
        }
    } catch (\Exception $e) {
        // Ignore, the meta data is informational only.
    }
    return $info;
}

try {
    assertAdministratorSession();
    if ("POST" === $_SERVER['REQUEST_METHOD']) {
        $filename = sanitizeDebFilename(strval(
            $_FILES['file']['name'] ?? ""
        ));
        $info = storeUploadedDeb($filename);
        sendJson($info);
    }
    if (isset($_GET['check'])) {
        // Session check for the upload page.
        $session = &\OMV\Session::getInstance();
        sendJson([
            "authenticated" => true,
            "username" => strval($session->getUsername())
        ]);
    }
    // Serve the self-contained upload page.
    sendUploadPage();
} catch (\Exception $e) {
    sendJson(null, 500, $e->getMessage());
}

/**
 * Print the self-contained HTML upload page.
 */
function sendUploadPage()
{
    header("Content-Type: text/html; charset=UTF-8");
    header("Cache-Control: no-cache, must-revalidate");
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OMV - Package Upload</title>
<style>
  :root { color-scheme: light dark; }
  * { box-sizing: border-box; }
  body {
    font-family: Roboto, "Helvetica Neue", Arial, "PingFang SC",
      "Microsoft YaHei", sans-serif;
    margin: 0; min-height: 100vh; display: flex;
    align-items: flex-start; justify-content: center;
    background: #f5f5f5; color: #212121;
  }
  @media (prefers-color-scheme: dark) {
    body { background: #1e1e1e; color: #e0e0e0; }
    .card { background: #2a2a2a !important; }
    .hint { color: #9e9e9e !important; }
    .drop { border-color: #616161 !important; }
    .drop.over { background: #333 !important; }
  }
  .card {
    background: #fff; margin: 3rem 1rem; padding: 2rem;
    border-radius: 8px; max-width: 640px; width: 100%;
    box-shadow: 0 2px 8px rgba(0,0,0,.15);
  }
  h1 { font-size: 1.3rem; margin: 0 0 .25rem; }
  .sub { font-size: .85rem; color: #616161; margin-bottom: 1.25rem; }
  .hint { font-size: .85rem; color: #757575; margin: 1rem 0; }
  .drop {
    border: 2px dashed #9e9e9e; border-radius: 8px;
    padding: 2rem 1rem; text-align: center; cursor: pointer;
    transition: background .15s;
  }
  .drop.over { background: #eeeeee; }
  input[type=file] { display: none; }
  .bar {
    height: 6px; background: #e0e0e0; border-radius: 3px;
    margin: 1rem 0; overflow: hidden; display: none;
  }
  .bar > div {
    height: 100%; width: 0; background: #1976d2;
    transition: width .2s;
  }
  #log { font-size: .85rem; white-space: pre-wrap; word-break: break-all; }
  .ok { color: #2e7d32; }
  .err { color: #c62828; }
  .file { margin-top: .5rem; }
  button {
    margin-top: 1rem; padding: .5rem 1.25rem; font-size: .9rem;
    border: none; border-radius: 4px; cursor: pointer;
    background: #1976d2; color: #fff;
  }
  button:disabled { background: #9e9e9e; cursor: default; }
</style>
</head>
<body>
<div class="card">
  <h1>OMV · <span id="t1">Package upload</span></h1>
  <div class="sub"><span id="user"></span></div>
  <div class="drop" id="drop">
    <span id="t2">Click to select .deb file(s) or drag &amp; drop here</span>
    <input type="file" id="file" accept=".deb,application/vnd.debian.binary-package" multiple>
  </div>
  <div class="bar" id="bar"><div id="fill"></div></div>
  <div id="log"></div>
  <div class="hint" id="t3">Files are stored in /tmp on the NAS. After uploading, go to System → Package Manager → Local deb to install them. Files in /tmp are cleaned up automatically by the system. Max. size: 25 MiB.</div>
  <button id="pick" type="button"><span id="t4">Select files</span></button>
</div>
<script>
(function () {
  var zh = (navigator.language || "").toLowerCase().indexOf("zh") === 0;
  function T(id, en, cn) {
    var el = document.getElementById(id);
    if (el && zh) { el.textContent = cn; }
  }
  if (!zh) {
    // English is the default text; nothing to swap.
  } else {
    document.title = "OMV - 上传安装包";
  }
  T("t1", "", "上传安装包");
  T("t2", "", "点击选择 .deb 文件，或拖拽到此处");
  T("t3", "", "文件将上传到 NAS 的 /tmp 目录。上传完成后，请到 系统 → 软件包管理器 → 本地 deb 页面点击刷新并安装。/tmp 中的文件由系统定期自动清理。大小上限：25 MiB。");
  T("t4", "", "选择文件");

  var drop = document.getElementById("drop");
  var input = document.getElementById("file");
  var bar = document.getElementById("bar");
  var fill = document.getElementById("fill");
  var log = document.getElementById("log");
  var pick = document.getElementById("pick");

  function line(text, cls) {
    var div = document.createElement("div");
    if (cls) { div.className = cls; }
    div.textContent = text;
    log.appendChild(div);
    return div;
  }
  function fmtSize(n) {
    var u = ["B", "KiB", "MiB", "GiB"], i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i ? n.toFixed(1) : n) + " " + u[i];
  }
  function upload(file) {
    return new Promise(function (resolve) {
      var head = line(file.name + " (" + fmtSize(file.size) + ") ... ");
      var fd = new FormData();
      fd.append("file", file, file.name);
      var xhr = new XMLHttpRequest();
      bar.style.display = "block"; fill.style.width = "0";
      xhr.upload.addEventListener("progress", function (e) {
        if (e.lengthComputable) {
          fill.style.width = Math.round(100 * e.loaded / e.total) + "%";
        }
      });
      xhr.addEventListener("load", function () {
        var msg, ok = false, pkg = "";
        try {
          var res = JSON.parse(xhr.responseText);
          if (200 === xhr.status && res.response && res.response.package) {
            ok = true;
            pkg = res.response.package + " " + res.response.version +
              " (" + res.response.architecture + ")";
          }
          msg = (res.error && res.error.message) ||
            (200 === xhr.status ? "" : xhr.status + " " + xhr.statusText);
        } catch (e) {
          msg = xhr.status + " " + xhr.statusText;
        }
        if (ok) {
          head.className = "ok";
          head.textContent = head.textContent.replace(" ... ", "") +
            "  ✓ " + pkg;
        } else {
          head.className = "err";
          head.textContent = head.textContent.replace(" ... ", "") +
            "  ✗ " + msg;
        }
        resolve();
      });
      xhr.addEventListener("error", function () {
        head.className = "err";
        head.textContent = head.textContent.replace(" ... ", "") +
          "  ✗ " + (zh ? "网络错误" : "network error");
        resolve();
      });
      xhr.open("POST", "pkgupload.php");
      xhr.send(fd);
    });
  }
  function handleFiles(files) {
    log.textContent = "";
    pick.disabled = true; drop.style.pointerEvents = "none";
    var chain = Promise.resolve();
    Array.prototype.forEach.call(files, function (f) {
      chain = chain.then(function () { return upload(f); });
    });
    chain.then(function () {
      bar.style.display = "none";
      pick.disabled = false; drop.style.pointerEvents = "";
      line(zh ? "请回到 系统 → 软件包管理器 → 本地 deb 页面，点击刷新按钮后安装。"
              : "Go to System → Package Manager → Local deb, click reload and install.");
    });
  }
  pick.addEventListener("click", function () { input.click(); });
  drop.addEventListener("click", function () { input.click(); });
  input.addEventListener("change", function () {
    if (input.files.length) { handleFiles(input.files); }
    input.value = "";
  });
  ["dragenter", "dragover"].forEach(function (ev) {
    drop.addEventListener(ev, function (e) {
      e.preventDefault(); drop.classList.add("over");
    });
  });
  ["dragleave", "drop"].forEach(function (ev) {
    drop.addEventListener(ev, function (e) {
      e.preventDefault(); drop.classList.remove("over");
    });
  });
  drop.addEventListener("drop", function (e) {
    if (e.dataTransfer.files.length) { handleFiles(e.dataTransfer.files); }
  });
  // Verify the OMV session up-front.
  fetch("pkgupload.php?check=1", { credentials: "same-origin" })
    .then(function (r) { return r.json(); })
    .then(function (res) {
      var name = res.response && res.response.username;
      document.getElementById("user").textContent = (zh ? "登录用户：" : "Logged in as: ") + name;
    })
    .catch(function () {
      document.getElementById("user").textContent =
        zh ? "⚠ 未登录或会话已过期，请先登录 OpenMediaVault 网页界面。"
           : "⚠ Not logged in or session expired. Please log in to the OpenMediaVault web interface first.";
    });
})();
</script>
</body>
</html>
<?php
}
