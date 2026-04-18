/**
 * Stateless HTML renderer for the local setup portal.
 */
#include "setup_portal_page.h"

#include <stdio.h>
#include <string.h>

static void html_escape_text(const char *src, char *dst, size_t dst_size) {
  const char *cursor;
  size_t used = 0;

  if (dst == NULL || dst_size == 0) {
    return;
  }

  dst[0] = '\0';
  if (src == NULL) {
    return;
  }

  for (cursor = src; *cursor != '\0' && used + 1 < dst_size; ++cursor) {
    const char *replacement = NULL;

    switch (*cursor) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        replacement = "&quot;";
        break;
      case '\'':
        replacement = "&#39;";
        break;
      default:
        dst[used++] = *cursor;
        dst[used] = '\0';
        continue;
    }

    while (*replacement != '\0' && used + 1 < dst_size) {
      dst[used++] = *replacement++;
    }
    dst[used] = '\0';
  }
}

static void send_chunk_if_not_empty(httpd_req_t *req, const char *text) {
  if (req == NULL || text == NULL || text[0] == '\0') {
    return;
  }

  httpd_resp_sendstr_chunk(req, text);
}

const char *lm_ctrl_setup_portal_history_target(const char *uri) {
  if (uri == NULL) {
    return NULL;
  }

  if (strcmp(uri, "/controller") == 0 || strcmp(uri, "/controller-logo-clear") == 0) {
    return "/#controller-section";
  }

  if (strcmp(uri, "/cloud") == 0 ||
      strcmp(uri, "/cloud-refresh") == 0 ||
      strcmp(uri, "/cloud-machine") == 0 ||
      strcmp(uri, "/bbw") == 0) {
    return "/#cloud-section";
  }

  return NULL;
}

esp_err_t lm_ctrl_setup_portal_send_page(
  httpd_req_t *req,
  const lm_ctrl_setup_portal_view_t *view,
  const char *history_target
) {
  if (req == NULL || view == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req, "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<link rel=\"icon\" href=\"data:,\">"
    "<title>La Marzocco Controller Setup</title>"
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#120d0a;color:#f4efe7;padding:24px;}"
    "main{max-width:560px;margin:0 auto;}h1{margin:0 0 8px;font-size:28px;}h2{margin:0 0 8px;font-size:20px;color:#ffc685;}p{color:#d5c1ae;}"
    "form{background:#261a13;border:1px solid #926333;border-radius:20px;padding:18px;margin-top:18px;}"
    "label{display:block;margin:14px 0 6px;color:#ffc685;font-weight:600;}"
    "input,select{width:100%;box-sizing:border-box;padding:12px 14px;border-radius:14px;border:1px solid #5d4128;background:#1a120d;color:#f4efe7;}"
    "button{margin-top:18px;padding:12px 18px;border:none;border-radius:14px;background:#ffc685;color:#120d0a;"
    "font-weight:700;cursor:pointer;}pre{white-space:pre-wrap;background:#1a120d;padding:16px;border-radius:16px;"
    "border:1px solid #5d4128;} .banner{color:#ffc685;font-weight:700;margin:12px 0;} .section-note{margin-top:6px;font-size:14px;color:#d5c1ae;}"
    ".secondary{background:#3a281d;color:#f4efe7;} .danger{background:#7f3328;color:#fff7f2;} .machine-pill{margin-top:12px;padding:12px 14px;border-radius:14px;background:#1a120d;border:1px solid #5d4128;}"
    ".button-row{display:flex;gap:12px;flex-wrap:wrap;} .button-row button{flex:1 1 180px;}"
    ".topnav{display:flex;gap:10px;flex-wrap:wrap;margin:18px 0 8px;}.topnav a{padding:10px 14px;border-radius:999px;border:1px solid #5d4128;background:#261a13;color:#f4efe7;text-decoration:none;font-weight:600;}"
    ".topnav a:hover{border-color:#926333;color:#ffc685;} .preset-anchor{scroll-margin-top:24px;}</style></head><body><main>");
  httpd_resp_sendstr_chunk(req, "<h1>Controller Setup</h1><p>Use this page to store the La Marzocco cloud account and the home Wi-Fi settings on the controller. The Wi-Fi scan fills the SSID automatically.</p>");
  if (view->banner_html[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<div class=\"banner\">");
    httpd_resp_sendstr_chunk(req, view->banner_html);
    httpd_resp_sendstr_chunk(req, "</div>");
  }
  httpd_resp_sendstr_chunk(req, "<pre>");
  httpd_resp_sendstr_chunk(req, view->status_html);
  httpd_resp_sendstr_chunk(req, "</pre>");
  httpd_resp_sendstr_chunk(req, "<p class=\"section-note\">Use <strong>");
  httpd_resp_sendstr_chunk(req, view->local_url_html);
  httpd_resp_sendstr_chunk(req, "</strong> in your home network. The router IP can change after reconnects or flashes.</p>");
  httpd_resp_sendstr_chunk(req, "<nav class=\"topnav\"><a href=\"#controller-section\">Controller</a><a href=\"#network-section\">Network</a><a href=\"#cloud-section\">Cloud</a><a href=\"#presets-section\">Presets</a><a href=\"#advanced-section\">Advanced</a></nav>");

  httpd_resp_sendstr_chunk(req, "<form id=\"controller-section\" class=\"preset-anchor\" method=\"post\" action=\"/controller\"><h2>Controller</h2>");
  httpd_resp_sendstr_chunk(req, "<label for=\"hostname\">Controller Hostname</label><input id=\"hostname\" name=\"hostname\" maxlength=\"32\" value=\"");
  send_chunk_if_not_empty(req, view->hostname_html);
  httpd_resp_sendstr_chunk(req, "\">");
  httpd_resp_sendstr_chunk(req, "<label for=\"language\">Controller Language</label><select id=\"language\" name=\"language\">");
  httpd_resp_sendstr_chunk(req, "<option value=\"en\"");
  if (view->info.language == CTRL_LANGUAGE_EN) {
    httpd_resp_sendstr_chunk(req, " selected");
  }
  httpd_resp_sendstr_chunk(req, ">English</option>");
  httpd_resp_sendstr_chunk(req, "<option value=\"de\"");
  if (view->info.language == CTRL_LANGUAGE_DE) {
    httpd_resp_sendstr_chunk(req, " selected");
  }
  httpd_resp_sendstr_chunk(req, ">Deutsch</option></select>");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Language only affects the on-device controller UI. English stays the default.</div><button type=\"submit\">Save Controller Settings</button></form>");

  httpd_resp_sendstr_chunk(req, "<form class=\"preset-anchor\"><h2>Header Logo</h2>");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Current header: <strong id=\"header-logo-state\">");
  httpd_resp_sendstr_chunk(req, view->info.has_custom_logo ? "Custom logo installed" : "Default text");
  httpd_resp_sendstr_chunk(req, "</strong></div>");
  httpd_resp_sendstr_chunk(req, "<label for=\"logo-file\">Local SVG File</label><input id=\"logo-file\" type=\"file\" accept=\".svg,image/svg+xml\">");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Optional controller setting. The project does not ship official La Marzocco logos. Uploaded SVGs are rasterized in your browser, stored locally on this controller, and shown only on the device display.</div>");
  httpd_resp_sendstr_chunk(req, "<div class=\"button-row\"><button type=\"button\" id=\"logo-upload-button\">Upload Logo</button>");
  httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\" formaction=\"/controller-logo-clear\" formmethod=\"post\" onclick=\"return confirm('Remove the custom controller logo and fall back to the text header?');\">Remove Logo</button></div>");
  httpd_resp_sendstr_chunk(req, "<div id=\"logo-upload-status\" class=\"section-note\"></div></form>");

  httpd_resp_sendstr_chunk(req, "<form id=\"network-section\" class=\"preset-anchor\" method=\"post\" action=\"/wifi\"><h2>Network</h2>");
  httpd_resp_sendstr_chunk(req, "<button type=\"button\" id=\"scan-button\">Scan Nearby Networks</button>");
  httpd_resp_sendstr_chunk(req, "<label for=\"network-list\">Nearby Networks</label><select id=\"network-list\"><option value=\"\">Select from scan results</option></select>");
  httpd_resp_sendstr_chunk(req, "<label for=\"ssid\">Home Wi-Fi SSID</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required value=\"");
  send_chunk_if_not_empty(req, view->ssid_html);
  httpd_resp_sendstr_chunk(req, "\">");
  httpd_resp_sendstr_chunk(req, "<label for=\"password\">Wi-Fi Password</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\">");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Saving Wi-Fi starts a reconnect attempt immediately. Leave the password blank to keep the stored password for the same SSID.</div><button type=\"submit\">Save Wi-Fi And Connect</button></form>");

  httpd_resp_sendstr_chunk(req, "<form id=\"cloud-section\" class=\"preset-anchor\" method=\"post\" action=\"/cloud\"><h2>La Marzocco Cloud</h2>");
  httpd_resp_sendstr_chunk(req, "<label for=\"cloud_username\">Account E-Mail</label><input id=\"cloud_username\" name=\"cloud_username\" type=\"email\" maxlength=\"95\" value=\"");
  send_chunk_if_not_empty(req, view->cloud_user_html);
  httpd_resp_sendstr_chunk(req, "\">");
  httpd_resp_sendstr_chunk(req, "<label for=\"cloud_password\">Account Password</label><input id=\"cloud_password\" name=\"cloud_password\" type=\"password\" maxlength=\"127\">");
  httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Saving verifies the account and loads the machine list from La Marzocco Cloud. It does not restart networking.</div><button type=\"submit\">Save Cloud Account And Load Machines</button></form>");

  if (view->info.has_cloud_credentials) {
    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/cloud-refresh\"><h2>Cloud Machines</h2>");
    if (view->info.has_machine_selection) {
      httpd_resp_sendstr_chunk(req, "<div class=\"machine-pill\">Selected: ");
      send_chunk_if_not_empty(req, view->selected_machine_html);
      httpd_resp_sendstr_chunk(req, "</div>");
    } else {
      httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">No machine selected yet.</div>");
    }
    httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\">Reload Machine List</button></form>");
  }

  if (view->fleet_count > 0) {
    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/cloud-machine\"><h2>Select Machine</h2>");
    httpd_resp_sendstr_chunk(req, "<label for=\"machine_serial\">Machine</label><select id=\"machine_serial\" name=\"machine_serial\" required>");
    for (size_t i = 0; i < view->fleet_count; ++i) {
      char machine_serial_html[96];
      char machine_name_html[160];
      char machine_model_html[96];

      html_escape_text(view->fleet[i].serial, machine_serial_html, sizeof(machine_serial_html));
      html_escape_text(view->fleet[i].name, machine_name_html, sizeof(machine_name_html));
      html_escape_text(view->fleet[i].model, machine_model_html, sizeof(machine_model_html));

      httpd_resp_sendstr_chunk(req, "<option value=\"");
      send_chunk_if_not_empty(req, machine_serial_html);
      httpd_resp_sendstr_chunk(req, "\"");
      if (view->info.has_machine_selection && strcmp(view->info.machine_serial, view->fleet[i].serial) == 0) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">");
      if (machine_name_html[0] != '\0') {
        httpd_resp_sendstr_chunk(req, machine_name_html);
      } else {
        httpd_resp_sendstr_chunk(req, "Unnamed machine");
      }
      if (machine_model_html[0] != '\0') {
        httpd_resp_sendstr_chunk(req, " · ");
        httpd_resp_sendstr_chunk(req, machine_model_html);
      }
      if (machine_serial_html[0] != '\0') {
        httpd_resp_sendstr_chunk(req, " · ");
        httpd_resp_sendstr_chunk(req, machine_serial_html);
      }
      httpd_resp_sendstr_chunk(req, "</option>");
    }
    httpd_resp_sendstr_chunk(req, "</select><div class=\"section-note\">The selected machine and communication key are stored locally on the controller.</div><button type=\"submit\">Use Selected Machine</button></form>");
  }

  if ((view->dashboard_feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) != 0) {
    char bbw_dose_1[24];
    char bbw_dose_2[24];

    snprintf(
      bbw_dose_1,
      sizeof(bbw_dose_1),
      "%.1f",
      (double)((view->dashboard_loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_1) != 0 ? view->dashboard_values.bbw_dose_1_g : 32.0f)
    );
    snprintf(
      bbw_dose_2,
      sizeof(bbw_dose_2),
      "%.1f",
      (double)((view->dashboard_loaded_mask & LM_CTRL_MACHINE_FIELD_BBW_DOSE_2) != 0 ? view->dashboard_values.bbw_dose_2_g : 34.0f)
    );

    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/bbw\"><h2>Brew By Weight</h2>");
    httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">Shown because the cloud dashboard reports brew by weight support for the selected machine.</div>");
    httpd_resp_sendstr_chunk(req, "<label for=\"bbw_mode\">Mode</label><select id=\"bbw_mode\" name=\"bbw_mode\">");
    httpd_resp_sendstr_chunk(req, "<option value=\"Dose1\"");
    if (view->dashboard_values.bbw_mode == CTRL_BBW_MODE_DOSE_1) {
      httpd_resp_sendstr_chunk(req, " selected");
    }
    httpd_resp_sendstr_chunk(req, ">Dose 1</option>");
    httpd_resp_sendstr_chunk(req, "<option value=\"Dose2\"");
    if (view->dashboard_values.bbw_mode == CTRL_BBW_MODE_DOSE_2) {
      httpd_resp_sendstr_chunk(req, " selected");
    }
    httpd_resp_sendstr_chunk(req, ">Dose 2</option>");
    httpd_resp_sendstr_chunk(req, "<option value=\"Continuous\"");
    if (view->dashboard_values.bbw_mode == CTRL_BBW_MODE_CONTINUOUS) {
      httpd_resp_sendstr_chunk(req, " selected");
    }
    httpd_resp_sendstr_chunk(req, ">Continuous</option></select>");
    httpd_resp_sendstr_chunk(req, "<label for=\"bbw_dose_1\">Dose 1 Target (g)</label><input id=\"bbw_dose_1\" name=\"bbw_dose_1\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, bbw_dose_1);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label for=\"bbw_dose_2\">Dose 2 Target (g)</label><input id=\"bbw_dose_2\" name=\"bbw_dose_2\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, bbw_dose_2);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<div class=\"section-note\">These values are cloud-only and will also be available on the round controller when BBW is supported.</div><button type=\"submit\">Save Brew By Weight Settings</button></form>");
  }

  httpd_resp_sendstr_chunk(req, "<div id=\"presets-section\" class=\"preset-anchor\"></div><p class=\"section-note\">Preset edits here only change the stored preset definitions. They do not load onto the controller and do not send anything to the machine.</p>");
  for (int preset_index = 0; preset_index < CTRL_PRESET_COUNT; ++preset_index) {
    char preset_title[24];
    char preset_slot_text[4];
    char preset_default_name[CTRL_PRESET_NAME_LEN];
    char preset_display_name[CTRL_PRESET_NAME_LEN];
    char preset_display_name_html[96];
    char preset_name_html[96];
    char preset_name_placeholder_html[96];
    char temperature_text[24];
    char infuse_text[24];
    char pause_text[24];
    char bbw_dose_1_text[24];
    char bbw_dose_2_text[24];
    const ctrl_preset_t *preset = &view->presets[preset_index];

    ctrl_preset_default_name(preset_index, preset_default_name, sizeof(preset_default_name));
    ctrl_preset_display_name(preset, preset_index, preset_display_name, sizeof(preset_display_name));
    html_escape_text(preset_display_name, preset_display_name_html, sizeof(preset_display_name_html));
    html_escape_text(preset->name, preset_name_html, sizeof(preset_name_html));
    html_escape_text(preset_default_name, preset_name_placeholder_html, sizeof(preset_name_placeholder_html));
    snprintf(preset_title, sizeof(preset_title), "Preset %d", preset_index + 1);
    snprintf(preset_slot_text, sizeof(preset_slot_text), "%d", preset_index);
    snprintf(temperature_text, sizeof(temperature_text), "%.1f", (double)preset->values.temperature_c);
    snprintf(infuse_text, sizeof(infuse_text), "%.1f", (double)preset->values.infuse_s);
    snprintf(pause_text, sizeof(pause_text), "%.1f", (double)preset->values.pause_s);
    snprintf(bbw_dose_1_text, sizeof(bbw_dose_1_text), "%.1f", (double)preset->values.bbw_dose_1_g);
    snprintf(bbw_dose_2_text, sizeof(bbw_dose_2_text), "%.1f", (double)preset->values.bbw_dose_2_g);

    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/preset\"><h2>");
    httpd_resp_sendstr_chunk(req, preset_title);
    httpd_resp_sendstr_chunk(req, "</h2><div class=\"section-note\">Current display name: ");
    httpd_resp_sendstr_chunk(req, preset_display_name_html);
    httpd_resp_sendstr_chunk(req, "</div><input type=\"hidden\" name=\"preset_slot\" value=\"");
    httpd_resp_sendstr_chunk(req, preset_slot_text);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label for=\"preset_name_");
    httpd_resp_sendstr_chunk(req, preset_slot_text);
    httpd_resp_sendstr_chunk(req, "\">Name</label><input id=\"preset_name_");
    httpd_resp_sendstr_chunk(req, preset_slot_text);
    httpd_resp_sendstr_chunk(req, "\" name=\"preset_name\" maxlength=\"32\" placeholder=\"");
    send_chunk_if_not_empty(req, preset_name_placeholder_html);
    httpd_resp_sendstr_chunk(req, "\" value=\"");
    send_chunk_if_not_empty(req, preset_name_html);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label>Temperature (C)</label><input name=\"temperature_c\" type=\"number\" min=\"80\" max=\"103\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, temperature_text);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label>Prebrewing In (s)</label><input name=\"infuse_s\" type=\"number\" min=\"0\" max=\"9\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, infuse_text);
    httpd_resp_sendstr_chunk(req, "\">");
    httpd_resp_sendstr_chunk(req, "<label>Prebrewing Out (s)</label><input name=\"pause_s\" type=\"number\" min=\"0\" max=\"9\" step=\"0.1\" value=\"");
    httpd_resp_sendstr_chunk(req, pause_text);
    httpd_resp_sendstr_chunk(req, "\">");
    if ((view->dashboard_feature_mask & LM_CTRL_MACHINE_FEATURE_BBW) != 0) {
      httpd_resp_sendstr_chunk(req, "<label>BBW Mode</label><select name=\"bbw_mode\">");
      httpd_resp_sendstr_chunk(req, "<option value=\"Dose1\"");
      if (preset->values.bbw_mode == CTRL_BBW_MODE_DOSE_1) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">Dose 1</option><option value=\"Dose2\"");
      if (preset->values.bbw_mode == CTRL_BBW_MODE_DOSE_2) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">Dose 2</option><option value=\"Continuous\"");
      if (preset->values.bbw_mode == CTRL_BBW_MODE_CONTINUOUS) {
        httpd_resp_sendstr_chunk(req, " selected");
      }
      httpd_resp_sendstr_chunk(req, ">Continuous</option></select>");
      httpd_resp_sendstr_chunk(req, "<label>BBW Dose 1 (g)</label><input name=\"bbw_dose_1\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
      httpd_resp_sendstr_chunk(req, bbw_dose_1_text);
      httpd_resp_sendstr_chunk(req, "\">");
      httpd_resp_sendstr_chunk(req, "<label>BBW Dose 2 (g)</label><input name=\"bbw_dose_2\" type=\"number\" min=\"5\" max=\"100\" step=\"0.1\" value=\"");
      httpd_resp_sendstr_chunk(req, bbw_dose_2_text);
      httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req, "<button type=\"submit\">Save Preset</button></form>");
  }

  httpd_resp_sendstr_chunk(req, "<form id=\"advanced-section\" class=\"preset-anchor\"><h2>Advanced</h2><div class=\"section-note\">Use these actions when you want to restart the controller or wipe onboarding data.</div><div class=\"button-row\">");
  httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\" formaction=\"/reboot\" formmethod=\"post\">Reboot Controller</button>");
  httpd_resp_sendstr_chunk(req, "<button class=\"secondary\" type=\"submit\" formaction=\"/reset-network\" formmethod=\"post\" onclick=\"return confirm('Clear Wi-Fi, cloud account, and machine selection?');\">Reset Network Setup</button>");
  httpd_resp_sendstr_chunk(req, "<button class=\"danger\" type=\"submit\" formaction=\"/factory-reset\" formmethod=\"post\" onclick=\"return confirm('Factory reset the controller and erase presets?');\">Factory Reset</button>");
  httpd_resp_sendstr_chunk(req, "</div></form>");
  httpd_resp_sendstr_chunk(req, "<form><h2>Diagnostics</h2><div class=\"section-note\">Read-only controller and link state for debugging reconnect or sync issues.</div><pre>");
  httpd_resp_sendstr_chunk(req, view->debug_status_html);
  httpd_resp_sendstr_chunk(req, "</pre></form>");

  httpd_resp_sendstr_chunk(req,
    "<script>"
    "const scanButton=document.getElementById('scan-button');"
    "const networkList=document.getElementById('network-list');"
    "const ssidInput=document.getElementById('ssid');"
    "const logoFileInput=document.getElementById('logo-file');"
    "const logoUploadButton=document.getElementById('logo-upload-button');"
    "const logoUploadStatus=document.getElementById('logo-upload-status');"
    "const logoState=document.getElementById('header-logo-state');"
    "const LOGO_WIDTH=150;"
    "const LOGO_HEIGHT=26;"
    "const LOGO_VERSION=1;"
    "const setLogoStatus=(text,isError)=>{if(!logoUploadStatus){return;}logoUploadStatus.textContent=text||'';logoUploadStatus.style.color=isError?'#ffb4a8':'#d5c1ae';};"
    "const readSvgText=(file)=>new Promise((resolve,reject)=>{const reader=new FileReader();reader.onload=()=>resolve(typeof reader.result==='string'?reader.result:'');reader.onerror=()=>reject(new Error('Could not read the SVG file.'));reader.readAsText(file);});"
    "const loadSvgImage=(svgText)=>new Promise((resolve,reject)=>{const url=URL.createObjectURL(new Blob([svgText],{type:'image/svg+xml'}));const image=new Image();image.onload=()=>{URL.revokeObjectURL(url);resolve(image);};image.onerror=()=>{URL.revokeObjectURL(url);reject(new Error('Could not decode the SVG file.'));};image.src=url;});"
    "const bytesToBase64=(bytes)=>{let binary='';const chunkSize=0x8000;for(let i=0;i<bytes.length;i+=chunkSize){binary+=String.fromCharCode(...bytes.subarray(i,i+chunkSize));}return btoa(binary);};"
    "const rasterizeSvgToLvgl=(image)=>{const canvas=document.createElement('canvas');canvas.width=LOGO_WIDTH;canvas.height=LOGO_HEIGHT;const ctx=canvas.getContext('2d');if(!ctx){throw new Error('Canvas rendering is not available.');}ctx.clearRect(0,0,LOGO_WIDTH,LOGO_HEIGHT);const sourceWidth=image.naturalWidth||image.width;const sourceHeight=image.naturalHeight||image.height;if(!sourceWidth||!sourceHeight){throw new Error('The SVG has no usable size.');}const scale=Math.min(LOGO_WIDTH/sourceWidth,LOGO_HEIGHT/sourceHeight);const drawWidth=sourceWidth*scale;const drawHeight=sourceHeight*scale;ctx.drawImage(image,(LOGO_WIDTH-drawWidth)/2,(LOGO_HEIGHT-drawHeight)/2,drawWidth,drawHeight);const rgba=ctx.getImageData(0,0,LOGO_WIDTH,LOGO_HEIGHT).data;const output=new Uint8Array(LOGO_WIDTH*LOGO_HEIGHT*3);for(let i=0,j=0;i<rgba.length;i+=4,j+=3){const r=rgba[i];const g=rgba[i+1];const b=rgba[i+2];const a=rgba[i+3];output[j]=((r&0xF8)|((g>>5)&0x07));output[j+1]=(((g&0x1C)<<3)|((b>>3)&0x1F));output[j+2]=a;}return output;};"
    "scanButton.addEventListener('click',async()=>{"
      "scanButton.disabled=true;scanButton.textContent='Scanning...';"
      "try{const response=await fetch('/wifi-scan');const data=await response.json();"
        "networkList.innerHTML='<option value=\"\">Select from scan results</option>';"
        "(data.ssids||[]).forEach((ssid)=>{const option=document.createElement('option');option.value=ssid;option.textContent=ssid;networkList.appendChild(option);});"
        "scanButton.textContent=(data.ssids||[]).length?'Scan Again':'No Networks Found';"
      "}catch(error){scanButton.textContent='Scan Failed';}"
      "finally{scanButton.disabled=false;}"
    "});"
    "networkList.addEventListener('change',()=>{if(networkList.value){ssidInput.value=networkList.value;}});"
    "logoUploadButton.addEventListener('click',async()=>{const file=logoFileInput&&logoFileInput.files?logoFileInput.files[0]:null;if(!file){setLogoStatus('Choose an SVG file first.',true);return;}if(!/\\.svg$/i.test(file.name)&&file.type!=='image/svg+xml'){setLogoStatus('Only SVG uploads are supported.',true);return;}logoUploadButton.disabled=true;logoUploadButton.textContent='Uploading...';setLogoStatus('',false);try{const svgText=await readSvgText(file);const image=await loadSvgImage(svgText);const logoBytes=rasterizeSvgToLvgl(image);const response=await fetch('/controller-logo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({version:LOGO_VERSION,width:LOGO_WIDTH,height:LOGO_HEIGHT,data:bytesToBase64(logoBytes)})});const payload=await response.json().catch(()=>({ok:false,message:'Unexpected response from controller.'}));if(!response.ok||!payload.ok){throw new Error(payload.message||'Upload failed.');}if(logoState){logoState.textContent='Custom logo installed';}setLogoStatus(payload.message||'Custom logo saved.',false);logoFileInput.value='';}catch(error){setLogoStatus(error&&error.message?error.message:'Upload failed.',true);}finally{logoUploadButton.disabled=false;logoUploadButton.textContent='Upload Logo';}});"
    "</script>");
  if (history_target != NULL) {
    httpd_resp_sendstr_chunk(req, "<script>history.replaceState(null,'','");
    httpd_resp_sendstr_chunk(req, history_target);
    httpd_resp_sendstr_chunk(req, "');</script>");
  }
  httpd_resp_sendstr_chunk(req, "</main></body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}
