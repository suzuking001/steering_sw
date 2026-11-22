'use strict';
const $ = id => document.getElementById(id);
const clone = obj => JSON.parse(JSON.stringify(obj));
let config, saved, csrf = '', dirty = false, currentStatus, activeTab = 'buttons';
let histories = [[], []], learningButton = null, polling = false, lastReadingAt = -Infinity;
let capturedReading = null, applyingCapture = false, previewingConfig = false;
let pollTimer, readingTimer, liveMonitorVisible = !('IntersectionObserver' in window);
let lastStatusAt = -Infinity, pollStartedAt = 0, samplingPaused = false;
let chartClock, chartCursor = null;
const freshReadingMs = 1500;
const statusTimeoutMs = 3000;
const chartWindowMs = 30000;
let renderedLogs = '';
let saving = false;
let macroDraft = null, macroDeleting = false, macroGroup = 'macros';
let wifiDirty = false, wifiSaving = false;
let firmwareRequest = false, firmwareTarget = '';
let testPending = false;
const isTouchEditor = navigator.maxTouchPoints > 0;
const inputModeClient = Array.from(crypto.getRandomValues(new Uint8Array(8)),
  byte => byte.toString(16).padStart(2, '0')).join('');
let inputModeRequested = false, inputModeRequest = null, inputModeDesired = false;
let focusTimer, inputModeHeartbeat;
const keyboardFields = ['wifi-ssid', 'wifi-password', 'button-name-input', 'ble-name-input'];
const fieldKeyboard = $('field-keyboard');
const fieldKeyboardHome = fieldKeyboard.parentElement;
let keyboardField, keyboardTimer, keyboardLayout;
let messageTimer;
const presetIds = ['maps','youtube','siri','lock','app_list'];
const settingsGroups = {
  inputs: ['入力設定', ['channels'], renderInputs],
  general: ['動作設定', ['name', 'outputEnabled', 'timing'], renderGeneral],
  ble: ['Bluetooth設定', ['ble'], renderBleFields],
  acc: ['ACC設定', ['acc'], renderAcc],
};
const configKeys = ['channels','buttons','macros','functions','functionOrder','name','outputEnabled','timing','ble','acc'];
const equal = (a, b) => JSON.stringify(a) === JSON.stringify(b);
function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key,value] of Object.entries(attrs)) {
    if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (key === 'class') node.className = value;
    else if (key === 'checked' || key === 'disabled') node[key] = value;
    else node.setAttribute(key, value);
  }
  for (const child of children.flat()) if (child !== null && child !== undefined) node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  return node;
}
function note(message, error = false) {
  const node = $('message'); node.textContent = message; node.className = error ? 'error' : ''; node.hidden = false;
  clearTimeout(messageTimer); messageTimer = setTimeout(() => { node.hidden = true; }, error ? 14000 : 7000);
}
function setText(id, value) {
  const node = $(id);
  if (node.textContent !== value) node.textContent = value;
}
function hasChanges() { return dirty || hasMacroEdits() || wifiDirty || previewingConfig || saving || wifiSaving; }
function unsavedParts() {
  const labels = Object.values(settingsGroups).filter(group => group[1].some(changed)).map(group => group[0]);
  return [previewingConfig && '読み込んだ設定', changed('buttons') && 'ボタン割当', (changed('macros') || changed('functions') || changed('functionOrder') || hasMacroEdits()) && '機能割当',
    ...labels, wifiDirty && 'Wi-Fi'].filter(Boolean);
}
function changed(key) {
  if (key === 'buttons' || key === 'macros' || key === 'functions') {
    return config && (config[key].length !== saved[key].length ||
      config[key].some(item => !equal(item, savedItem(key, item))));
  }
  return !equal(config?.[key], saved?.[key]);
}
function refreshDirty() {
  dirty = configKeys.some(changed);
  for (const controls of document.querySelectorAll('.config-save')) controls.update();
  $('config-preview').hidden = !previewingConfig;
  $('save-preview').disabled = $('cancel-preview').disabled = saving;
  $('reset-functions').disabled = saving || previewingConfig;
  $('cancel-wifi').disabled = !wifiDirty || wifiSaving;
  $('apply-learn').disabled = saving || applyingCapture;
  $('save-macro').disabled = $('discard-macro').disabled = $('macro-editor-fields').disabled = saving;
}
function setDirty() { refreshDirty(); }
function saveControls(label, hasEdits, apply, undo, id = '') {
  const status = el('span', {class:'muted',role:'status'});
  let error = '', working = false;
  const save = el('button', {type:'button',class:'save-item',...(id ? {id:'save-'+id} : {}),
    onclick:async()=>{
      error = ''; working = true; controls.update();
      try { note((await apply()).message); }
      catch (failure) { error = failure.message; }
      finally { working = false; controls.update(); }
    }}, label+'を保存');
  const cancel = el('button', {type:'button',class:'secondary cancel-item',...(id ? {id:'cancel-'+id} : {}),
    onclick:()=>{error = ''; undo();}}, '変更を取り消す');
  const controls = el('div',{class:'config-save'},el('div',{class:'actions'},save,cancel),status);
  controls.update = () => {
    const pending = hasEdits();
    save.disabled = cancel.disabled = !pending || saving || working || previewingConfig;
    const message = working ? '保存中…' : previewingConfig ? '読み込んだ設定の適用待ち' :
      pending ? error || '未保存' : '保存済み';
    if (status.textContent !== message) status.textContent = message;
    status.classList.toggle('error', !previewingConfig && pending && !!error);
  };
  controls.update();
  return controls;
}
function fieldsFor(keys) {
  return Object.fromEntries(keys.map(key => [key, config[key]]));
}
async function saveConfig(fields, replace = false) {
  if (saving) throw new Error('設定を保存中です。');
  if (currentStatus?.learning?.active) throw new Error('計測が終わってから保存してください。');
  const submitted = clone(fields);
  saving = true;
  refreshDirty();
  try {
    const result = replace ? await api('config','PUT',{...submitted, version:config.version, ssid:saved.ssid}) :
      await api('config','PATCH',submitted);
    Object.assign(saved, submitted);
    void poll();
    return result;
  } finally {
    saving = false;
    refreshDirty();
  }
}
async function saveSettings(keys, replace = false) {
  const result = await saveConfig(fieldsFor(keys), replace);
  $('device-name').textContent = saved.name;
  return result;
}
function replaceItem(items, id, item) {
  const index = items.findIndex(entry => entry.id === id);
  if (index < 0) return item ? [...items, item] : items;
  const next = [...items];
  next.splice(index, 1, ...(item ? [item] : []));
  return next;
}
function savedItem(key, item) { return saved[key].find(entry => entry.id === item.id); }
function cancelItem(key, item, render) {
  const original = savedItem(key, item);
  config[key] = replaceItem(config[key], item.id, original && clone(original));
  render();
  refreshDirty();
}
function feedback(id, message, kind = '') {
  $(id).textContent = message; $(id).className = 'form-feedback '+kind;
}
function field(label, obj, key, type = 'number', attrs = {}) {
  const input = el('input', {type, ...attrs});
  if (type === 'checkbox') input.checked = !!obj[key]; else input.value = obj[key];
  input.addEventListener(type === 'checkbox' ? 'change' : 'input', () => {
    obj[key] = type === 'checkbox' ? input.checked : type === 'number' ? input.valueAsNumber : input.value; setDirty();
  });
  return el('label', {class: type === 'checkbox' ? 'check' : ''}, ...(type === 'checkbox' ? [input,label] : [label,input]));
}
function selectField(label, values, value, onChange) {
  const select = el('select');
  for (const [id,name] of values) select.append(el('option', {value:id},name));
  select.value = String(value); select.addEventListener('change', () => onChange(select.value));
  return el('label', {},label,select);
}
async function api(path, method = 'GET', data, retryToken = true) {
  const options = {method, credentials:'same-origin', cache:'no-store', headers:{}};
  if (path === 'ble/input-mode' && data?.enabled === false) options.keepalive = true;
  const controller = new AbortController(); options.signal = controller.signal;
  let timeoutMs = 10000;
  if (method==='GET') {
    timeoutMs = path.startsWith('readings') ? freshReadingMs : path.startsWith('status') ? statusTimeoutMs : 4000;
  }
  const timeout = setTimeout(()=>controller.abort(),timeoutMs);
  if (method !== 'GET') options.headers['X-CSRF-Token'] = csrf;
  if (data !== undefined) { options.headers['Content-Type'] = 'application/json'; options.body = JSON.stringify(data); }
  let response, result;
  try { response = await fetch('/api/' + path, options); result = await response.json(); }
  catch(error) { throw new Error(error.name==='AbortError'?'本体の応答を待ちきれませんでした。入力は残っています。Wi-Fiの接続先を確認してください。':'本体と通信できません。入力は残っています。接続先を確認して再試行してください。'); }
  finally { clearTimeout(timeout); }
  if(response.status===403 && method!=='GET' && retryToken) {
    csrf=(await api('session')).csrf;
    return api(path,method,data,false);
  }
  if (!response.ok) {
    const error = new Error(result.error || '通信に失敗しました');
    error.status = response.status; throw error;
  }
  return result;
}
async function connectWorkspace() {
  $('opening').hidden = false; $('retry-connection').hidden = true;
  try { csrf = (await api('session')).csrf; await openWorkspace(); }
  catch(error) {
    csrf = ''; $('workspace').hidden = true; $('opening').hidden = false;
    $('opening-message').textContent = '本体へ接続できません。Wi-Fiの接続先を確認し、もう一度接続してください。';
    $('retry-connection').hidden = false;
  }
}
async function openWorkspace() {
  config = await api('config'); saved = clone(config); histories = [[],[]];
  chartClock = undefined; chartCursor = null; lastReadingAt = -Infinity;
  wifiDirty = false;
  $('wifi-password').value = '';
  $('show-wifi-password').checked = false;
  $('wifi-password').type = 'password';
  learningButton = null; previewingConfig = false; renderAll();
  $('message').hidden = true;
  $('opening').hidden = true; $('workspace').hidden = false;
  checkInputFocus();
  await poll();
}
function renderAll() {
  $('device-name').textContent = config.name;
  if(!wifiDirty) $('wifi-ssid').value = config.ssid;
  renderLive(); renderInputs(); renderButtons(); renderMacros(); renderSettings();
  renderBleFields();
  for (const [id, [label, keys, render]] of Object.entries(settingsGroups)) {
    $(id+'-save').replaceChildren(saveControls(label, () => keys.some(changed),
      () => id === 'inputs' ? saveInputs() : saveSettings(keys), () => {
        if (id === 'inputs') cancelInputUnits();
        for (const key of keys) config[key] = clone(saved[key]);
        render();
        if (id === 'inputs') renderButtons();
        refreshDirty();
        checkInputFocus();
      }, id));
  }
  refreshDirty();
  checkInputFocus();
}
function renderBleFields() {
  $('ble-fields').replaceChildren(
    field('端末に表示されるBluetooth名',config.ble,'name','text',{id:'ble-name-input',maxlength:29,autocomplete:'off'}),
    field('Bluetoothを有効にする',config.ble,'enabled','checkbox',{id:'ble-enabled-input'}));
  $('ble-name-input').closest('label').classList.add('wide');
}
function renderLive() {
  $('live-cards').replaceChildren(...config.channels.map((ch,i) => el('section',{class:'panel'},
    el('div',{class:'card-heading'},el('h3',{'data-channel-label':i},channelName(i)),el('span',{class:'badge'},'GPIO ' + [36,39][i])),
    el('div',{id:'reading-button-'+i,class:'read-button'},'—'),
    el('div',{class:'metrics'},...['ADC','mV','Ω（推定）'].map((name,j) => el('div',{class:'metric'},el('strong',{id:'metric-'+i+'-'+j},'—'),el('small',{},name)))),
    el('canvas',{id:'chart-'+i,'aria-label':channelName(i)+'の電圧 (mV)',role:'img'}),
    el('p',{id:'reading-detail-'+i,class:'muted'},'測定待ち'))));
}
const channelUnit = mode => ({adc:'ADC', mv:'mV', ohm:'Ω'}[mode]);
function channelName(index) {
  const key = 'KEY' + (index + 1);
  const name = config.channels[index].name.trim();
  return name && name !== key ? key + ' · ' + name : key;
}
function measuredValue(channel, reading) {
  if (channel.mode === 'adc') return reading.adc;
  if (channel.mode === 'mv') return reading.mv;
  if (!Number.isFinite(reading.adc) || !Number.isFinite(reading.mv) ||
      reading.adc >= 4090 || reading.mv >= channel.supplyMv - 20) return null;
  return channel.pullup * reading.mv / (channel.supplyMv - reading.mv);
}
function changedInputUnits() {
  return new Set(config.channels.flatMap((channel, index) => channel.mode !== saved.channels[index].mode ? [index] : []));
}
function buttonNeedsInputSave(button, units = changedInputUnits()) {
  return units.has(button.channel) || units.has(savedItem('buttons', button)?.channel);
}
async function saveInputs() {
  if (config.channels.some(channel =>
    ['pullup','supplyMv','idleCenter','idleTolerance','hysteresis'].some(key => !Number.isFinite(channel[key])))) {
    throw new Error('入力設定の数値と未押下の中心値を設定してください。');
  }
  const units = changedInputUnits();
  if (!units.size) {
    const result = await saveConfig(fieldsFor(['channels']));
    refreshChannelNames();
    return result;
  }
  const buttons = saved.buttons.filter(button => !units.has(button.channel) &&
    !config.buttons.some(draft => draft.id === button.id && units.has(draft.channel)));
  const recalibrated = config.buttons.filter(button => buttonNeedsInputSave(button, units));
  if (recalibrated.some(button => !Number.isFinite(button.center) || !Number.isFinite(button.tolerance))) {
    throw new Error('判定単位を変更した系統のボタンを学習し、許容幅を確認してください。');
  }
  buttons.push(...recalibrated);
  const outputBeforeSave = config.outputEnabled;
  const result = await saveConfig({channels:config.channels, buttons, outputEnabled:false});
  if (config.outputEnabled === outputBeforeSave) {
    config.outputEnabled = false;
    $('output-enabled-input').checked = false;
  }
  refreshChannelNames();
  refreshDirty();
  return result;
}
function refreshChannelNames() {
  for (const title of document.querySelectorAll('[data-channel-label]')) {
    title.textContent = channelName(Number(title.dataset.channelLabel));
  }
}
function cancelInputUnits(units = changedInputUnits()) {
  for (const button of config.buttons.filter(button => buttonNeedsInputSave(button, units))) {
    const original = savedItem('buttons', button);
    if (original) button.channel = original.channel;
    button.center = original?.center ?? null;
  }
}
function saveButton(button) {
  if (buttonNeedsInputSave(button)) throw new Error('判定単位を変更中です。本体設定の「入力設定を保存」で対象のボタンをまとめて反映してください。');
  if (!Number.isFinite(button.center) || !Number.isFinite(button.tolerance)) {
    throw new Error(button.name+'の中心値と許容幅を入力するか、学習してください。');
  }
  return saveConfig({buttons:replaceItem(saved.buttons, button.id, button)});
}
function hasFreshReading() {
  return performance.now() - lastReadingAt < freshReadingMs;
}
function setIdleFromReading(index, input) {
  const reading = currentStatus?.readings?.[index];
  if (!reading || !hasFreshReading()) {
    note('最新の測定値がありません。本体との接続を確認してから、もう一度押してください。', true);
    return;
  }
  const channel = config.channels[index];
  const value = measuredValue(channel, reading);
  if (!Number.isFinite(value)) {
    note(channelName(index)+'の現在値を選択中の判定単位で測定できません。', true);
    return;
  }
  channel.idleCenter = Number(value.toFixed(1));
  input.value = channel.idleCenter;
  setDirty();
  note(channelName(index)+'の未押下の中心値に '+channel.idleCenter+' '+channelUnit(channel.mode)+' を設定しました。');
}
function renderInputs() {
  $('input-cards').replaceChildren(...config.channels.map((channel, index) => {
    const idleCenter = field('未押下の中心値', channel, 'idleCenter', 'number', {min:0,step:'any'});
    const setIdle = el('button', {type:'button',class:'secondary',
      onclick:()=>setIdleFromReading(index,idleCenter.querySelector('input'))}, '現在値を設定する');
    const fields = el('div', {class:'fields'},
      field('系統名', channel, 'name', 'text', {maxlength:24}),
      selectField('判定単位', [['adc','ADC値'], ['mv','電圧 mV'], ['ohm','推定抵抗値 Ω']], channel.mode, value => {
        if (value === saved.channels[index].mode && !previewingConfig) {
          channel.mode = value;
          channel.idleCenter = saved.channels[index].idleCenter;
          cancelInputUnits(new Set([index]));
          renderButtons();
          refreshDirty();
        } else if (confirm('単位を変更すると、その系統の未押下範囲とボタンの中心値を測り直す必要があります。校正後、「入力設定を保存」でまとめて反映します。保存時にキー送信はOFFになります。')) {
          channel.mode = value;
          channel.idleCenter = null;
          for (const button of config.buttons.filter(button=>button.channel===index)) button.center = null;
          renderButtons();
          refreshDirty();
        }
        renderInputs();
      }),
      field('プルアップ抵抗 Ω', channel, 'pullup', 'number', {min:10,max:1000000,step:'any'}),
      field('供給電圧 mV', channel, 'supplyMv', 'number', {min:1000,max:3600,step:'any'}),
      el('div',{class:'field-with-action wide'},idleCenter,setIdle),
      field('未押下の許容幅 ±', channel, 'idleTolerance', 'number', {min:0.1,step:'any'}),
      field('ヒステリシス', channel, 'hysteresis', 'number', {min:0,step:'any'}));
    return el('section', {class:'panel'}, el('h3', {'data-channel-label':index}, channelName(index)), fields);
  }));
}
function openLearning(button) {
  learningButton = button;
  capturedReading = null;
  $('learn-instruction').hidden = $('learn-cards').hidden = false;
  $('learn-confirmation').hidden = $('apply-learn').hidden = true;
  $('learn-title').textContent = button.name + 'の学習';
  feedback('learn-feedback','キー送信を一時停止しています…');
  $('learn-cards').replaceChildren(...config.channels.map((_,index) => {
    const capture = el('button',{type:'button',id:'capture-'+index,disabled:true,
      onclick:()=>captureReading(index)}, '現在値を設定する');
    return el('section',{class:'learn-card'},el('h3',{},channelName(index)),
      el('strong',{id:'learn-mv-'+index,class:'learn-voltage'},'— mV'),
      el('p',{id:'learn-value-'+index,class:'muted'},'測定待ち'),capture);
  }));
  $('learn-dialog').showModal();
  checkInputFocus();
  renderLearning();
  poll();
}
function renderLearning() {
  if (!learningButton || !$('learn-dialog').open || capturedReading) return;
  const ready = currentStatus?.inputModePaused && hasFreshReading();
  for (let i=0; i<config.channels.length; i++) {
    const channel = config.channels[i], reading = currentStatus?.readings?.[i];
    const value = reading ? measuredValue(channel, reading) : null;
    setText('learn-mv-'+i, Number.isFinite(reading?.mv) ? Math.round(reading.mv) + ' mV' : '— mV');
    setText('learn-value-'+i, '判定値 ' + (Number.isFinite(value) ? Number(value.toFixed(1))+' '+channelUnit(channel.mode) : '—'));
    const full = !channelHasSpace(i, learningButton);
    const capture = $('capture-'+i);
    capture.disabled = !ready || !Number.isFinite(value) || full;
    setText('capture-'+i, full ? '登録上限（16個）' : '現在値を設定する');
  }
  setText('learn-feedback', ready ? '' :
    '測定値とキー送信の停止を確認しています…');
}
function captureReading(index) {
  if (!learningButton || $('capture-'+index).disabled || !hasFreshReading()) return;
  const channel = config.channels[index];
  const measured = measuredValue(channel, currentStatus.readings[index]);
  if (!Number.isFinite(measured)) return;
  const value = Number(measured.toFixed(1));
  capturedReading = {channel:index, center:value};
  $('learn-instruction').hidden = $('learn-cards').hidden = true;
  $('learn-confirmation').hidden = $('apply-learn').hidden = false;
  setText('learn-confirmation', channelName(index)+' · '+value+' '+channelUnit(channel.mode));
  const staged = previewingConfig || buttonNeedsInputSave({...learningButton, ...capturedReading});
  setText('apply-learn', staged ? '学習値を設定' : '適用して保存');
  feedback('learn-feedback', previewingConfig ? '設定の復元時に一緒に保存されます。' : staged ?
    '判定単位を変更中です。入力とボタンの校正後に「入力設定を保存」を押してください。' : '');
  $('apply-learn').focus();
}
async function applyLearning() {
  if (!capturedReading || !learningButton || applyingCapture || saving) return;
  const button = learningButton;
  const learned = {...button, ...capturedReading};
  const staged = previewingConfig || buttonNeedsInputSave(learned);
  const channel = config.channels[learned.channel];
  const stored = saved.channels[learned.channel];
  if (!staged && channel.mode === 'ohm' && (channel.pullup !== stored.pullup || channel.supplyMv !== stored.supplyMv)) {
    feedback('learn-feedback','入力設定を保存してから、抵抗値を取り直してください。','error');
    return;
  }
  applyingCapture = true;
  $('close-learn').disabled = true;
  refreshDirty();
  try {
    if (!staged) note((await saveButton(learned)).message);
    Object.assign(button, capturedReading);
    $('learn-dialog').close();
    renderButtons();
    refreshDirty();
    document.querySelector('[data-button-id="'+button.id+'"] .learn-button')?.focus();
    checkInputFocus();
  } catch (error) {
    feedback('learn-feedback',error.message,'error');
  } finally {
    applyingCapture = false;
    $('close-learn').disabled = false;
    refreshDirty();
  }
}
function channelHasSpace(channel, except) {
  return config.buttons.filter(button=>button !== except && button.channel===channel).length < 16;
}
const macroAction = id => presetIds.includes(id) ? id : 'macro:'+id;
function actionOptions(isLong) {
  return [['none','未割り当て'],...config.functions.filter(m=>isLong || m.id!=='passcode').map(m=>[m.id,m.name]),
    ...config.macros.map(m=>[macroAction(m.id),m.name])];
}
function actionSteps(settings, action) {
  if (action.startsWith('macro:')) return settings.macros.find(m=>m.id===action.slice(6))?.steps;
  return (settings.functions.find(m=>m.id===action) || settings.macros.find(m=>m.id===action))?.steps;
}
const confirmTestDigits = () => confirm('接続先のロックを解除して、メモの空欄を開いてください。テスト用に 0123456789 を送信します。');
async function testAction(action) {
  if (testPending) return;
  if (action==='test_digits' && !confirmTestDigits()) return;
  if ($('learn-dialog').open) throw new Error('学習画面を閉じてからテストしてください。');
  if (!equal(actionSteps(config, action), actionSteps(saved, action))) {
    throw new Error('機能の変更を保存するか、機能割当で編集中の内容をテストしてください。');
  }
  const result = await sendTest({action});
  if (result) note(result.message);
}
async function sendTest(payload) {
  if (testPending) return;
  testPending = true;
  try {
    // On touch browsers, tapping a button can leave the previous input focused.
    if (activeInputField()) document.activeElement.blur();
    clearTimeout(focusTimer);
    checkInputFocus();
    if (inputModeRequest || inputModeRequested) {
      note('キー送信の再開を待っています…');
      await inputModeRequest;
      if (!inputModeDesired) await syncInputMode();
    }
    if (inputModeDesired || $('learn-dialog').open) throw new Error('入力や学習を終了してからテストしてください。');
    if (inputModeRequested) throw new Error('キー送信の再開を確認できませんでした。接続を確認して再試行してください。');
    const result = await api('test', 'POST', payload);
    poll();
    return result;
  } finally {
    testPending = false;
  }
}
function renderButtons() {
  const openDetails = new Set([...document.querySelectorAll('#button-cards details[open]')].map(detail=>detail.closest('[data-button-id]').dataset.buttonId));
  $('button-cards').replaceChildren(...config.buttons.map(button => {
    const labels = config.channels.map((channel,index)=>[index, channelName(index)]);
    const unit = channelUnit(config.channels[button.channel].mode);
    const title = el('h3', {}, button.name);
    const nameField = field('ボタン名', button, 'name', 'text', {maxlength:32});
    nameField.querySelector('input').addEventListener('input', () => { title.textContent = button.name; });
    const channelField = selectField('入力系統',labels,button.channel,value=>{
      const next = Number(value);
      if (config.channels[button.channel].mode !== config.channels[next].mode) button.center = null;
      button.channel = next;
      setDirty(); renderButtons();
    });
    for (const option of channelField.querySelectorAll('option')) {
      option.disabled = !channelHasSpace(Number(option.value), button);
    }
    const actions = el('div',{class:'button-actions'});
    for (const [key,label] of [['shortAction','短押し'],['doubleAction','ダブルクリック'],['longAction','長押し']]) {
      const selected = button[key];
      const unavailable = key !== 'shortAction' && ['volume_up','volume_down'].includes(button.shortAction);
      const shown = selected.startsWith('macro:') && presetIds.includes(selected.slice(6)) ? selected.slice(6) : selected;
      const action = selectField(label,actionOptions(key==='longAction'),shown,value=>{
        button[key]=value;
        if (key==='shortAction' && ['volume_up','volume_down'].includes(value)) button.doubleAction=button.longAction='none';
        setDirty(); renderButtons();
      });
      action.querySelector('select').disabled = unavailable;
      const digitsTest = selected==='passcode';
      const test = el('button', {type:'button',class:'secondary',
        disabled: unavailable || selected==='none',
        'aria-label':digitsTest ? '数字入力をテスト' : label+'をテスト',
        onclick:()=>run(()=>testAction(digitsTest ? 'test_digits' : selected))}, digitsTest ? '数字テスト' : 'テスト');
      actions.append(el('div',{class:'field-with-action'},action,test));
    }
    const summary = el('span');
    const updateSummary = () => {
      const learned = Number.isFinite(button.center);
      summary.className = 'badge ' + (learned?'good':'warn');
      summary.textContent = learned ? channelName(button.channel)+' · '+button.center+' '+unit : '未学習';
    };
    updateSummary();
    const center = field('中心値',button,'center','number',{min:0,step:'any'});
    center.querySelector('input').addEventListener('input',updateSummary);
    const detail = el('details',{},el('summary',{},'入力系統・中心値・許容幅'),el('div',{class:'fields'},
      channelField, center,
      field('許容幅 ±',button,'tolerance','number',{min:0.1,step:'any'}),
      el('p',{class:'muted wide'},'単位 '+unit)));
    detail.open = openDetails.has(button.id);
    return el('section',{class:'button-card','data-button-id':button.id},
      el('div',{class:'card-heading'},title,
        el('button',{type:'button',class:'quiet',onclick:()=>run(async()=>{
          if (!confirm(button.name+'を削除しますか？')) return;
          if (!previewingConfig && savedItem('buttons', button)) {
            note((await saveConfig({buttons:replaceItem(saved.buttons, button.id)})).message);
          }
          config.buttons=replaceItem(config.buttons, button.id);
          renderButtons();refreshDirty();
        })},'削除')),
      nameField,
      el('div',{class:'button-learn-row'},summary,
        el('button',{type:'button',class:'learn-button',onclick:()=>openLearning(button)},'学習')),
      actions, detail, saveControls('ボタン', () => !equal(button, savedItem('buttons', button)),
        () => saveButton(button), () => cancelItem('buttons', button, renderButtons)));
  }));
}
const shortcutModifiers = [[1,'Control'],[4,'Option'],[2,'Shift'],[8,'Command']];
const rightModifiers = shortcutModifiers.map(([bit,label])=>[bit << 4,'右'+label]);
const allModifiers = [...shortcutModifiers,...rightModifiers];
const keyRange = (first,labels) => Array.from(labels,(label,index)=>[first+index,label]);
const functionKeys = (first,number) => [0,6].map(offset=>
  keyRange(first+offset,Array.from({length:6},(_,i)=>'F'+(number+offset+i))));
const shortcutRows = [
  keyRange(30,'1234567890'),
  ...['QWERTYUIOP','ASDFGHJKL','ZXCVBNM'].map(row=>Array.from(row,key=>[key.charCodeAt(0)-61,key])),
  [[41,'Esc'],[43,'Tab'],[57,'Caps Lock'],[44,'Space'],[40,'Enter'],[42,'⌫']],
  keyRange(73,['Insert','Home','Pg Up','Delete','End','Pg Down']),
  [[80,'←'],[82,'↑'],[81,'↓'],[79,'→']],
];
const extraShortcutRows = [
  ...functionKeys(58,1), ...functionKeys(104,13),
  keyRange(45,'-=[]\\'), keyRange(51,";'\`,./"),
  keyRange(70,['Print Screen','Scroll Lock','Pause']),
  [[50,'非US #/~'],[100,'非US \\/|'],[101,'Application'],[102,'Power']],
  keyRange(83,['Num Lock',...Array.from('/*-+',key=>'テンキー '+key)]),
  [[88,'テンキー Enter'],[99,'テンキー .'],[103,'テンキー =']],
  ...['789','456','1230'].map(row=>Array.from(row,key=>[key==='0' ? 98 : 88+Number(key),'テンキー '+key])),
];
const shortcutLabels = new Map([...shortcutRows,...extraShortcutRows].flat());
shortcutLabels.set(42,'Backspace');
let shortcutDraft, applyShortcut, shortcutTimer, shortcutListening = false;
function validShortcut({key,modifiers,globe}) {
  return key===0 ? modifiers>0 || globe : shortcutLabels.has(key);
}
function shortcutDisplay(step) {
  const {key, modifiers} = step;
  if (!validShortcut(step)) return '—';
  const parts = allModifiers
    .filter(([mask]) => (modifiers & mask)!==0).map(([,name]) => name);
  if (step.globe) parts.unshift('地球儀');
  if (key) parts.push(shortcutLabels.get(key));
  return parts.join('＋');
}
function showShortcutKeyboard(step, onApply) {
  clearTimeout(shortcutTimer);
  shortcutListening = false;
  applyShortcut = onApply;
  shortcutDraft = {...step};
  $('shortcut-feedback').textContent = '';
  $('shortcut-received').textContent = '';
  if (!$('shortcut-keys').childElementCount) {
    const keyButton = (value,label,kind,select) => el('button',{
      type:'button',class:'secondary',['data-'+kind]:value,'aria-pressed':'false',
      ...(value===42 && kind==='key' ? {'aria-label':'Backspace'} : {}),
      onclick:()=>{shortcutDraft=select();updateShortcutKeyboard();}
    },label);
    const rows = keys => keys.map(row=>el('div',{class:'shortcut-row'},row.map(([key,label])=>
      keyButton(key,label,'key',()=>({...shortcutDraft,key:shortcutDraft.key===key ? 0 : key})))));
    const modifiers = keys => keys.map(([bit,label])=>keyButton(bit,label,'modifier',()=>
      ({...shortcutDraft,modifiers:shortcutDraft.modifiers ^ bit})));
    const globe = keyButton('', '🌐 地球儀', 'globe',()=>{
      const next = {...shortcutDraft};
      if (next.globe) delete next.globe;
      else next.globe = true;
      return next;
    });
    $('shortcut-modifiers').replaceChildren(...modifiers(shortcutModifiers),globe);
    $('shortcut-keys').replaceChildren(...rows(shortcutRows),
      el('details',{id:'shortcut-more'},el('summary',{},'F1〜F24・テンキー・その他'),
        el('div',{class:'shortcut-row'},modifiers(rightModifiers)),
        ...rows(extraShortcutRows)));
  }
  $('shortcut-more').open = !!(shortcutDraft.modifiers & 240) ||
    !!$('shortcut-more').querySelector('[data-key="'+shortcutDraft.key+'"]');
  updateShortcutKeyboard();
  $('shortcut-dialog').showModal();
  $('shortcut-dialog').scrollTop = 0;
  checkInputFocus();
}
function updateShortcutKeyboard() {
  shortcutListening = false;
  setText('shortcut-selection', shortcutDisplay(shortcutDraft));
  for (const key of $('shortcut-keys').querySelectorAll('[data-key]')) {
    key.setAttribute('aria-pressed',+key.dataset.key===shortcutDraft.key);
  }
  $('shortcut-modifiers').querySelector('[data-globe]').setAttribute('aria-pressed',!!shortcutDraft.globe);
  for (const modifier of $('shortcut-dialog').querySelectorAll('[data-modifier]')) {
    modifier.setAttribute('aria-pressed',!!(shortcutDraft.modifiers & +modifier.dataset.modifier));
  }
  for (const id of ['shortcut-test','shortcut-delay','shortcut-apply']) $(id).disabled=!validShortcut(shortcutDraft);
}
async function testShortcut(delay) {
  if (testPending) return;
  const draft = shortcutDraft;
  const steps = [draft];
  if (delay) steps.unshift({type:'wait',ms:5000});
  const display = $('shortcut-feedback');
  clearTimeout(shortcutTimer);
  shortcutListening = !!(draft.key || draft.modifiers);
  $('shortcut-received').textContent = '';
  display.focus();
  display.textContent = '送信準備中…';
  try {
    await sendTest({steps});
    if (shortcutDraft !== draft) return;
    display.textContent = delay ? '5秒後に送信します。' : '送信を開始しました。';
    if (shortcutListening) shortcutTimer = setTimeout(()=>{
      if (shortcutDraft !== draft) return;
      shortcutListening=false;
      if (!$('shortcut-received').textContent) setText('shortcut-received','このページで受信なし');
    },delay ? 7000 : 2000);
  } catch (error) {
    if (shortcutDraft === draft) {
      shortcutListening = false;
      display.textContent = error.message;
    }
  }
}
function clearMacroReference(buttons, id) {
  return buttons.map(button => {
    const updated = {...button};
    for (const key of ['shortAction','doubleAction','longAction']) if (updated[key] === 'macro:'+id) updated[key] = 'none';
    return updated;
  });
}
const macroList = $('macro-list');
let draggedFunction, functionDropTarget;
function renderMacros() {
  const rows = new Map();
  for (const group of ['functions','macros']) for (const m of config[group]) {
    const action = group==='macros' ? macroAction(m.id) : m.id;
    rows.set(action,el('li',{draggable:'true','data-action':action},el('button',{
      type:'button',class:'secondary','data-macro-id':m.id,'data-group':group,'aria-haspopup':'dialog',onclick:()=>openMacro(m, group)
    }, m.name)));
  }
  const order = new Set([...config.functionOrder.split(','),...rows.keys()]);
  macroList.replaceChildren(...[...order].map(action=>rows.get(action)).filter(Boolean));
  $('add-macro').disabled = config.macros.length >= 16;
}
function markFunctionDrop(target) {
  if (target===functionDropTarget) return;
  functionDropTarget?.classList.remove('drop-target');
  functionDropTarget = target;
  target?.classList.add('drop-target');
}
macroList.addEventListener('dragstart',event=>{
  const row = event.target.closest('li');
  if (!row || saving) { event.preventDefault(); return; }
  draggedFunction = row;
  row.classList.add('dragging');
  event.dataTransfer.effectAllowed = 'move';
  event.dataTransfer.setData('text/plain',row.dataset.action);
});
macroList.addEventListener('dragover',event=>{
  if (!draggedFunction || saving) return;
  const row = event.target.closest('li');
  markFunctionDrop(row !== draggedFunction ? row : null);
  if (!functionDropTarget) return;
  event.preventDefault();
  event.dataTransfer.dropEffect = 'move';
});
macroList.addEventListener('dragleave',event=>{
  if (!macroList.contains(event.relatedTarget)) markFunctionDrop(null);
});
macroList.addEventListener('dragend',()=>{
  draggedFunction?.classList.remove('dragging');
  draggedFunction = null;
  markFunctionDrop(null);
});
macroList.addEventListener('drop',event=>{
  const target = event.target.closest('li');
  if (!draggedFunction || !target || target===draggedFunction || saving) return;
  event.preventDefault();
  const order = [...macroList.children].map(row=>row.dataset.action);
  const from = order.indexOf(draggedFunction.dataset.action);
  const to = order.indexOf(target.dataset.action);
  order.splice(to,0,order.splice(from,1)[0]);
  draggedFunction = null;
  markFunctionDrop(null);
  const previous = config.functionOrder;
  config.functionOrder = order.join(',');
  renderMacros();
  if (previewingConfig) { refreshDirty(); return; }
  void run(async()=>{
    try {
      await saveSettings(['functionOrder']);
      note('機能の並び順を保存しました。');
    } catch (error) {
      config.functionOrder = previous;
      renderMacros(); refreshDirty();
      throw error;
    }
  });
});
function hasMacroEdits() {
  return !!macroDraft && (macroDeleting || !equal(macroDraft, config[macroGroup].find(m=>m.id===macroDraft.id)));
}
function openMacro(m, group = 'macros') {
  macroDraft = clone(m);
  macroGroup = group;
  macroDeleting = false;
  $('delete-macro').hidden = group==='functions' || presetIds.includes(m.id) || !config.macros.some(item=>item.id===m.id);
  setText('save-macro', previewingConfig ? '下書きに保存' : '保存');
  renderMacroEditor();
  updateMacroDeletion();
  $('macro-dialog').showModal();
  $('macro-scroll').scrollTop = 0;
  $('macro-title').focus();
}
function macroFeedback(message = '', kind = '') {
  feedback('macro-feedback', message, kind);
  $('macro-feedback').hidden = !message;
}
function updateMacroDeletion() {
  $('macro-fields').hidden = macroDeleting;
  setText('delete-macro', macroDeleting ? '削除を取り消す' : 'この機能を削除');
  macroFeedback(macroDeleting ? '保存するとこの機能が削除され、割り当てたボタンは未割り当てになります。' : '');
}
function closeMacro() {
  const id = macroDraft.id;
  macroDraft = null;
  $('macro-dialog').close();
  $('macro-fields').replaceChildren();
  (document.querySelector('#macro-list [data-group="'+macroGroup+'"][data-macro-id="'+id+'"]') || $('add-macro')).focus();
  checkInputFocus();
}
async function saveMacro() {
  if (saving || !macroDraft) return;
  const m = macroDraft;
  if (!hasMacroEdits()) { closeMacro(); return; }
  const item = macroDeleting ? undefined : m;
  try {
    if (!previewingConfig) {
      const fields = {[macroGroup]:replaceItem(saved[macroGroup], m.id, item)};
      if (macroDeleting) {
        const buttons = clearMacroReference(saved.buttons, m.id);
        if (!equal(buttons, saved.buttons)) fields.buttons = buttons;
      }
      macroFeedback('保存中…');
      note((await saveConfig(fields)).message);
    }
    config[macroGroup] = replaceItem(config[macroGroup], m.id, item);
    if (macroDeleting) config.buttons = clearMacroReference(config.buttons, m.id);
    renderMacros();renderButtons();refreshDirty();closeMacro();
  } catch (error) {
    macroFeedback(error.message, 'error');
  }
}
function renderMacroEditor() {
  const m = macroDraft;
  const card = $('macro-fields');
  card.replaceChildren(field('機能名',m,'name','text',{maxlength:32}));
  const operations = [['chord','キーボード／ショートカット'],['media','メディアキー'],['text','文字列'],['wait','待機']];
  if (macroGroup==='functions' && m.id==='passcode') operations.push(['passcode','保存済みパスコード']);
  m.steps.forEach((s,index)=>{
    const operation = s.type;
    const fields=el('div',{class:'fields'},selectField('操作',operations,operation,value=>{
      m.steps[index]={type:value,...(value==='chord'?{key:30,modifiers:0}:value==='media'?{usage:205}:value==='text'?{text:'sample'}:value==='wait'?{ms:500}:{})};renderMacroEditor();
    }));
    if(operation==='chord') {
      const selection = el('span',{class:'shortcut-value'},shortcutDisplay(s));
      fields.append(el('div',{class:'field-with-action wide'},selection,el('button',{
        type:'button',class:'secondary open-shortcut',onclick:()=>showShortcutKeyboard(s,chosen=>{
          m.steps[index]=chosen;
          renderMacroEditor();
          card.children[index+1].querySelector('.open-shortcut')?.focus();
        })
      },'キーボードで選ぶ')));
      if(s.type==='chord') {
        const keyField = field('HIDキー番号（10進数）',s,'key','number',{min:0,max:115,step:1});
        const modifiers = el('div',{class:'modifiers'});
        for(const [bit,label] of allModifiers) {
          const check=el('input',{type:'checkbox',value:bit,checked:!!(s.modifiers&bit),onchange:event=>{s.modifiers=event.target.checked?s.modifiers|bit:s.modifiers&~bit;}});
          modifiers.append(el('label',{class:'check'},check,label));
        }
        const advanced=el('details',{class:'wide'},el('summary',{},'キー番号と修飾キー'),keyField,modifiers);
        for (const event of ['input','change']) advanced.addEventListener(event,()=>{selection.textContent=shortcutDisplay(s);});
        fields.append(advanced);
      }
    }
    if(operation==='media')fields.append(selectField('送信キー',[[233,'音量＋'],[234,'音量－'],[226,'消音'],[205,'再生／一時停止'],[181,'次の曲'],[182,'前の曲'],[183,'停止']],s.usage,value=>{s.usage=Number(value);}));
    if(s.type==='text') { const f=field('入力文字列（ASCII）',s,'text','text',{maxlength:128});f.classList.add('wide');fields.append(f); }
    if(s.type==='wait')fields.append(field('待機時間 ms',s,'ms','number',{min:0,max:10000,step:1}));
    const tools=el('div',{class:'step-toolbar'},el('button',{class:'secondary',disabled:index===0,onclick:()=>{[m.steps[index-1],m.steps[index]]=[m.steps[index],m.steps[index-1]];renderMacroEditor();}},'↑'),
      el('button',{class:'secondary',disabled:index===m.steps.length-1,onclick:()=>{[m.steps[index+1],m.steps[index]]=[m.steps[index],m.steps[index+1]];renderMacroEditor();}},'↓'),
      el('button',{class:'secondary',onclick:()=>{m.steps.splice(index,1);renderMacroEditor();}},'削除'));
    card.append(el('div',{class:'step'},fields,tools));
  });
  card.append(el('div',{class:'actions'},el('button',{class:'secondary',disabled:m.steps.length>=32,onclick:()=>{m.steps.push({type:'wait',ms:500});renderMacroEditor();}},'ステップ追加'),
    el('button',{class:'secondary test-macro',onclick:async()=>{
      try {
        const sample = m.steps.some(step=>step.type==='passcode');
        if (sample && !confirmTestDigits()) return;
        const steps = sample ? m.steps.map(step=>step.type==='passcode' ? {type:'text',text:'0123456789'} : step) : m.steps;
        const result=await sendTest({steps});
        if (result && macroDraft===m) macroFeedback(result.message, 'good');
      } catch (error) {
        if (macroDraft===m) macroFeedback(error.message, 'error');
      }
    }},'機能をテスト')));
}
function renderSettings() {
  renderGeneral();
  renderAcc();
}
function renderGeneral() {
  const fields=$('general-fields');
  fields.replaceChildren(field('設定名',config,'name','text',{maxlength:32}),field('キー送信を有効にする',config,'outputEnabled','checkbox',{id:'output-enabled-input'}));
  for(const [key,label,min,max] of [['debounceMs','安定判定 ms',5,250],['longMs','長押し ms',400,5000],['doubleMs','ダブルクリック間隔 ms',100,600],['repeatDelayMs','音量の連続開始 ms',200,3000],['repeatMs','音量の連続間隔 ms',50,1000]])fields.append(field(label,config.timing,key,'number',{min,max,step:1}));
}
function renderAcc() {
  $('acc-fields').replaceChildren(field('ACC連動を有効にする',config.acc,'enabled','checkbox'),
    field('OFFしきい値 mV',config.acc,'thresholdMv','number',{min:100,max:3000,step:1}),field('OFF安定判定 ms',config.acc,'debounceMs','number',{min:100,max:10000,step:1}));
}
function pruneChart(data, now) {
  const oldest = data.findIndex(point=>now-point.at<=chartWindowMs);
  if (oldest!==0) data.splice(0,oldest===-1?data.length:oldest);
}
function drawChart(i, now = chartClock ? chartClock.at + performance.now() - chartClock.receivedAt : performance.now()) {
  const canvas=$('chart-'+i);if(!canvas||activeTab!=='settings')return;
  const rect=canvas.getBoundingClientRect();if(!rect.width)return;
  const ratio=Math.min(window.devicePixelRatio||1,2),width=rect.width,height=170;
  const pixelsWide=Math.round(width*ratio),pixelsHigh=Math.round(height*ratio);
  if (canvas.width!==pixelsWide) canvas.width=pixelsWide;
  if (canvas.height!==pixelsHigh) canvas.height=pixelsHigh;
  const ctx=canvas.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,width,height);
  const data=histories[i];
  pruneChart(data,now);
  let max=3300;
  for (const point of data) max=Math.max(max,point.mv);
  const left=43,top=12,bottom=height-22,right=width-10;
  const y=value=>bottom-value/max*(bottom-top);
  ctx.font='10px sans-serif';ctx.textAlign='right';
  for(let t=0;t<=4;t++){const val=max*t/4;ctx.strokeStyle='#dfe9ef';ctx.beginPath();ctx.moveTo(left,y(val));ctx.lineTo(right,y(val));ctx.stroke();ctx.fillStyle='#607788';ctx.fillText(Math.round(val).toLocaleString(),left-5,y(val)+4);}
  ctx.strokeStyle='#137d83';ctx.lineWidth=2;ctx.beginPath();let previous=-Infinity;
  for (const {at,mv} of data) {
    if (!Number.isFinite(mv)) { previous=-Infinity; continue; }
    const x=right-(now-at)/chartWindowMs*(right-left);
    if (at-previous>100) ctx.moveTo(x,y(mv)); else ctx.lineTo(x,y(mv));
    previous=at;
  }
  ctx.stroke();
  ctx.fillStyle='#607788';ctx.textAlign='left';ctx.fillText('−30秒',left,height-5);ctx.textAlign='right';ctx.fillText('現在 · mV',right,height-5);
}
function renderLiveStatus(readings) {
  const ohmNotes={adc_saturated:'ADC上限 · Ω測定不可',supply_limit:'供給電圧に近い · Ω測定不可',
    adc_error:'ADC読み取りエラー',voltage_unavailable:'校正電圧を取得できません',waiting:'測定待ち'};
  const display = value => Number.isFinite(value) ? value.toLocaleString(undefined,{maximumFractionDigits:1}) : '—';
  for (const [i,r] of readings.entries()) {
    setText('reading-button-'+i,r.button);
    setText('metric-'+i+'-0',display(r.adc));
    setText('metric-'+i+'-1',display(r.mv));
    setText('metric-'+i+'-2',Number.isFinite(r.ohm) ? display(r.ohm) : '測定不可');
    const detail = ['生ADC '+display(r.raw), '判定単位 '+channelUnit(r.mode), ohmNotes[r.ohmState]];
    setText('reading-detail-'+i,detail.filter(Boolean).join(' · '));
  }
}
function showLiveConnection(message) {
  setText('live-connection',message);
  $('live-connection').hidden = !message;
}
function appendChartPoint(deviceAt, voltages) {
  const at = chartClock.at - ((chartClock.deviceAt - deviceAt) >>> 0);
  for (const [i, mv] of voltages.entries()) {
    const latest = histories[i].at(-1);
    if (!latest || latest.at < at) histories[i].push({at,mv});
  }
  chartCursor = deviceAt;
}
function acceptReadings(s) {
  const receivedAt = performance.now();
  const elapsed = chartClock ? (s.uptimeMs - chartClock.deviceAt) >>> 0 : 0;
  if (elapsed > 0x7fffffff) {
    chartClock = undefined; chartCursor = null; histories = [[],[]];
  }
  const at = chartClock ? chartClock.at + elapsed : receivedAt;
  chartClock = {at, receivedAt, deviceAt:s.uptimeMs};
  const sampleAge = Number.isInteger(s.sampleAtMs) ? (s.uptimeMs - s.sampleAtMs) >>> 0 : Infinity;
  const age = sampleAge + (receivedAt - pollStartedAt);
  lastReadingAt = receivedAt - age;
  clearTimeout(readingTimer);
  const freshness = !Number.isFinite(sampleAge) ? '測定待ち' : sampleAge >= freshReadingMs ? '本体の測定更新待ち' :
                    age >= freshReadingMs ? '測定値の受信に時間がかかりました' : '';
  showLiveConnection(freshness);
  if (age < freshReadingMs) {
    readingTimer = setTimeout(() => {
      showLiveConnection('新しい測定値を待っています');
      renderLearning();
    },freshReadingMs-age+1);
  }
  if (activeTab!=='settings') return;
  for (const [deviceAt,...voltages] of s.history ?? []) appendChartPoint(deviceAt,voltages);
  if (chartCursor === null && age < freshReadingMs) appendChartPoint(s.sampleAtMs,s.readings.map(r=>r.mv));
  if (monitorActive()) {
    renderLiveStatus(s.readings);
    for (let i=0;i<histories.length;i++) drawChart(i,at);
  } else {
    for (const history of histories) pruneChart(history,at);
  }
}
function renderOperationLogs(s) {
  setText('network-status',(s.wifiConnected?'Wi-Fi '+s.ip+' / '+s.rssi+' dBm':'既存Wi-Fi 未接続')+(s.apActive?' · 設定用AP '+s.apName+' / '+s.apIp:'')+' · 稼働 '+Math.floor(s.uptimeMs/1000)+'秒 · 空きRAM '+Math.round(s.freeHeap/1024)+'KB'+(s.httpIdleTimeouts>0?' · 要求なしで閉じたHTTP接続 '+s.httpIdleTimeouts+'件':''));
  setText('learn-status',s.learning.active?'学習中：キー送信を停止しています。残り '+(s.learning.remainingMs/1000).toFixed(1)+'秒':s.busy?'マクロ送信中':'入力監視中');
  if (!Array.isArray(s.logs)) return;
  const key=JSON.stringify(s.logs);
  if (key===renderedLogs) return;
  renderedLogs=key;
  $('logs').replaceChildren(...s.logs.slice().reverse().map(log=>el('li',{},el('time',{},(log.at/1000).toFixed(1)+'s'),log.message)));
}
function wifiStatus(s) {
  const ssid=s.wifiSavedSsid ?? saved.ssid;
  const configured=s.wifiConfigured ?? !!ssid;
  $('wifi-saved-state').textContent=configured?'保存済み：'+ssid+(s.wifiPasswordSet?'（パスワード登録済み）':'（パスワードなし）'):'Wi-Fi接続先：未設定';
  $('wifi-saved-state').className='save-state '+(configured?'good':'');
  const state=s.wifiState || (s.wifiConnected?'connected':configured?'connecting':'unconfigured');
  const descriptions={unconfigured:'ルーターへの接続先は未設定です。設定用Wi-Fiで操作できます。',connecting:'保存済みのWi-Fiへ接続中です…',getting_ip:'Wi-Fiに接続しました。IPアドレスを取得中です…',connected:'接続成功：'+(s.wifiConnectedSsid||ssid),failed:'ルーターに接続できていません。設定は保存されています。'+(s.wifiError||'SSID・パスワード・電波の状態を確認してください。')};
  setText('wifi-connection-state',descriptions[state]||'接続を確認しています…');
  $('wifi-connection-state').className='connection-state '+(state==='connected'?'good':state==='failed'?'error':'');
  $('wifi-status-wait').hidden = true;
  $('wifi-disconnects').hidden = !(s.wifiDisconnectCount > 0);
  if (s.wifiDisconnectCount > 0) setText('wifi-disconnects',`起動後のルーター切断 ${s.wifiDisconnectCount}回 · 前回の理由コード ${s.wifiLastDisconnectReason}`);
  $('wifi-reconnect').disabled=!configured||wifiSaving;
  const url=s.wifiLanUrl || (s.wifiConnected&&s.ip&&s.ip!=='0.0.0.0'?'http://'+s.ip+'/':'');
  $('wifi-lan-access').hidden=!url;
  if(url){$('wifi-lan-url').textContent=url;$('wifi-lan-url').href=url;$('wifi-lan-detail').textContent='PCを「'+(s.wifiConnectedSsid||ssid)+'」と同じネットワークにつなぎ、このURLをブラウザーで開いてください。'+(s.mdnsReady?' http://steering-sw.local/ も利用できます。':'');}
  if(!wifiDirty&&!wifiSaving){
    $('wifi-open-network').checked=configured&&!s.wifiPasswordSet;
    $('wifi-password').placeholder=s.wifiPasswordSet?'登録済み（変更しない場合は入力不要）':'Wi-Fiパスワードを入力';
  }
  if(!wifiDirty&&!wifiSaving) feedback('wifi-feedback',configured?'保存済みの設定です。接続結果は上の表示をご確認ください。':'Wi-Fi設定を入力し、保存してください。',configured?'good':'');
}
function renderBle(s, statusReceived = true) {
  const ble = s?.ble;
  const ready = s?.bleReady;
  const waiting = s?.bleConnected && ble?.encrypted && !ready;
  const partial = ready && (!ble?.keyboardReady || !ble?.mediaReady);
  const state = !ble ? (s?.bleSuspended ? '一時停止中' : '通信待ち') : !ble.enabled ? '無効' : s.bleSuspended ? '一時停止中' :
    ready ? (partial ? '一部操作可能' : '操作可能') : waiting ? 'キー通知待ち' : s.bleConnected ? '暗号化待ち' : ble.advertising ? '接続待ち' : '未接続';
  setText('ble-state',!statusReceived && ble ? state+'（前回）' : state);
  $('ble-state').className = 'badge '+(ready && statusReceived?'good':'warn');
  if (ble?.name) $('guide-ble-name').textContent = ble.name;
  $('ble-advertising').textContent = ble ? (s.bleConnected?'接続済み':ble.advertising?'受付中':'停止中') : '—';
  $('ble-encryption').textContent = s?.bleConnected ? (ble?.encrypted?'完了':'準備中') : '—';
  $('ble-keyboard').textContent = s?.bleConnected ? (ble?.keyboardReady?'使用可能':'準備中') : '—';
  $('ble-media').textContent = s?.bleConnected ? (ble?.mediaReady?'使用可能':ble?.protocolMode==='boot'?'互換モードでは使用不可':'準備中') : '—';
  $('ble-paired').textContent = Number.isInteger(ble?.bonds) ? ble.bonds+'台' : '—';
  $('reconnect-ble').disabled = !statusReceived || !ble?.enabled || s.bleSuspended;
  $('forget-ble').disabled = !statusReceived || !ble;
  const details = [];
  if (ble?.advertising && ble.advertisingIntervalMs > 0) details.push('接続待ち信号 '+ble.advertisingIntervalMs+' ms間隔');
  if (s?.bleConnected && ble?.protocolMode) {
    details.push(ble.protocolMode==='boot'?'入力方式 互換モード':'入力方式 標準モード');
    details.push(ble.reportMapRead?'今回の接続でレポートマップの読み取りあり':'今回の接続でレポートマップの読み取りなし');
  }
  if (ble?.securityError > 0) details.push('暗号化エラー 0x'+ble.securityError.toString(16).toUpperCase());
  if (ble?.disconnectReason > 0) details.push('前回の切断コード 0x'+ble.disconnectReason.toString(16).toUpperCase());
  if (ble?.serviceRefreshes > 0) details.push(`サービス情報の再確認を要求 ${ble.serviceRefreshes}回（端末の確認応答 ${ble.serviceRefreshConfirmations ?? 0}回）`);
  if (Number.isInteger(ble?.automaticReconnects)) details.push('自動再接続の試行 '+ble.automaticReconnects+'回');
  if (s?.bleConnected && !ready && ble?.automaticRetryExhausted) details.push('自動再試行の上限に達しました');
  else if (s?.bleConnected && !ready && ble?.automaticRetryUsed) details.push('自動再試行中');
  const diagnostics = $('ble-diagnostic');
  diagnostics.textContent = details.join(' · ');
  diagnostics.hidden = !details.length;
}
function renderFirmware(s) {
  const firmware = s?.firmware;
  if (!firmware) return;
  const {version, availableVersion, stage, received, size, bootChecking} = firmware;
  const updating = ['downloading','verifying','restarting'].includes(stage);
  const ready = s.wifiConnected && !bootChecking && !updating && stage!=='checking' && !firmwareRequest;
  $('firmware-repository').textContent = firmware.repository || '—';
  $('firmware-version').textContent = version;
  $('firmware-available').hidden = !availableVersion;
  $('firmware-available').textContent = availableVersion ? '配布バージョン '+availableVersion : '';
  $('firmware-check').disabled = !ready;
  $('firmware-install').hidden = stage!=='available';
  $('firmware-install').disabled = !ready;
  $('firmware-progress').hidden = !updating || !size;
  $('firmware-progress').value = size ? Math.floor(Math.min(received, size)*100/size) : 0;

  let message = firmware.message;
  let kind = stage==='failed' ? 'error' : '';
  if (bootChecking) message = '更新後の起動を確認しています…';
  else if (firmwareTarget && stage==='idle') {
    message = version===firmwareTarget ? '更新が完了しました。新しい画面を表示するにはページを再読み込みしてください。' :
      '更新後に再接続しました。現在のバージョンを確認して、必要に応じて再試行してください。';
    kind = version===firmwareTarget ? 'good' : 'error';
  } else if (!s.wifiConnected && !updating) message = '本体をインターネットに接続できるWi-Fiにつないでください。';
  else if (!message) message = 'GitHub Releasesから更新できます。';
  if (stage==='failed' && firmware.detail) message += '（'+firmware.detail+'）';
  if ($('firmware-status').textContent!==message) feedback('firmware-status',message,kind);
}

async function requestFirmwareUpdate(version) {
  if (firmwareRequest) return;
  const requestFeedback = $('firmware-request-feedback');
  requestFeedback.hidden = true;
  if (version && hasChanges()) {
    const parts = unsavedParts();
    requestFeedback.textContent = parts.length ? '未保存の変更（'+parts.join('・')+'）を各項目で保存するか、取り消してから更新してください。' :
      '設定の保存が完了してから更新してください。';
    requestFeedback.hidden = false;
    return;
  }
  firmwareRequest = true;
  firmwareTarget = version || '';
  renderFirmware(currentStatus);
  try {
    await api(version ? 'firmware/install' : 'firmware/check', 'POST', version ? {version} : undefined);
    await poll();
  } catch (error) {
    requestFeedback.textContent = error.message;
    requestFeedback.hidden = false;
  } finally {
    firmwareRequest = false;
    renderFirmware(currentStatus);
  }
}
async function saveWifi() {
  if(wifiSaving) throw new Error('Wi-Fi設定を保存中です。');
  const ssid=$('wifi-ssid').value, password=$('wifi-password').value, open=$('wifi-open-network').checked;
  if(open&&password) throw new Error('パスワードを入力した場合は「パスワードなし」のチェックを外してください。');
  const payload={ssid};if(password||open||!ssid)payload.password=open||!ssid?'':password;
  wifiSaving=true;$('save-wifi').disabled=true;refreshDirty();
  feedback('wifi-feedback','保存しています…');
  try{
    const result=await api('wifi','PUT',payload);
    config.ssid=saved.ssid=ssid;
    wifiDirty=$('wifi-ssid').value!==ssid||$('wifi-password').value!==password||$('wifi-open-network').checked!==open;
    refreshDirty();feedback('wifi-feedback',wifiDirty?'送信した設定は保存済みです。その後の入力変更は未保存です。':'Wi-Fi設定を保存しました。接続成功かどうかは上の接続状態をご確認ください。','good');
    return result;
  }catch(error){feedback('wifi-feedback','保存を確認できませんでした：'+error.message,'error');throw error;}
  finally{wifiSaving=false;$('save-wifi').disabled=false;refreshDirty();}
}
function renderInputMode() {
  const help = $('input-mode-help');
  let message = '';
  if (isTouchEditor && inputModeDesired && currentStatus?.ble?.enabled!==false && !$('learn-dialog').open) {
    message = currentStatus?.inputModePaused ?
      '文字入力中はステアリングからのキー送信を一時停止しています。' :
      '文字入力のためキー送信を一時停止しています…';
  }
  if (help.textContent !== message) help.textContent = message;
  help.hidden = !message;
}
function syncInputMode() {
  if (inputModeRequest) return inputModeRequest;
  if (!csrf || (!inputModeDesired && !inputModeRequested)) return;
  const enabled = inputModeDesired;
  // A lost response can still mean that the device received the pause.
  if (enabled) inputModeRequested = true;
  inputModeRequest = api('ble/input-mode', 'PUT', {enabled, client:inputModeClient}).then(() => {
    inputModeRequested = enabled;
  }).catch(error => {
    if (enabled && inputModeDesired) note(error.message, true);
    // The device expires its pause even if the resume request is lost.
  }).finally(() => {
    inputModeRequest = null;
    if (inputModeDesired !== enabled) syncInputMode();
  });
  return inputModeRequest;
}
function setInputFocus(focused) {
  if (inputModeDesired === focused) return;
  inputModeDesired = focused;
  clearInterval(inputModeHeartbeat);
  if (focused) inputModeHeartbeat = setInterval(() => {
    checkInputFocus();
    syncInputMode();
  }, 4000);
  renderInputMode();
  syncInputMode();
}
function activeInputField() {
  const active = document.activeElement;
  const field = fieldKeyboard.contains(active) ? keyboardField : active;
  if (field?.matches('#workspace input:is([type="text"], [type="password"], [type="number"], :not([type])), #workspace textarea') &&
      !field.disabled && !field.readOnly && !field.closest('[hidden]')) return field;
  return null;
}
function checkInputFocus() {
  const focused = isTouchEditor && !document.hidden ? activeInputField() : null;
  updateFieldKeyboard(focused && keyboardFields.includes(focused.id) ? focused : null);
  setInputFocus(!document.hidden && (!!focused || $('learn-dialog').open));
}
function updateFieldKeyboard(field) {
  if (keyboardField === field) return;
  clearTimeout(keyboardTimer);
  keyboardField = field;
  fieldKeyboard.hidden = true;
  if (!field) {
    fieldKeyboardHome.append(fieldKeyboard);
    return;
  }
  keyboardLayout = 'lower';
  field.closest('label').after(fieldKeyboard);
  renderFieldKeyboard();
  // Safari can report the software keyboard's viewport change after an animation.
  keyboardTimer = setTimeout(() => {
    keyboardTimer = undefined;
    showFieldKeyboard();
  }, 450);
}
function showFieldKeyboard() {
  if (!keyboardField || keyboardTimer !== undefined) return;
  const viewport = window.visualViewport;
  const nativeVisible = viewport && viewport.scale <= 1.1 &&
    document.documentElement.clientHeight - viewport.height > 140;
  const panel = fieldKeyboard;
  if (nativeVisible || document.hidden) {
    panel.hidden = true;
  } else if (panel.hidden) {
    panel.hidden = false;
    panel.scrollIntoView({block:'nearest'});
  }
}
function renderFieldKeyboard() {
  const rows = {
    lower: ['1234567890', 'qwertyuiop', 'asdfghjkl', 'zxcvbnm'],
    upper: ['1234567890', 'QWERTYUIOP', 'ASDFGHJKL', 'ZXCVBNM'],
    symbols: ["!@#$%^&*()", "-_=+[]{}", "|\\;:'\"<>`", ",.?/~"]
  };
  const key = (label, action, special = false) => el('button', {
    type:'button', class:special?'secondary':'', onclick:action
  }, label);
  const panel = fieldKeyboard;
  const tools = el('div', {class:'field-keyboard-row'});
  const changeLayout = (layout, label) => key(label, () => {
    keyboardLayout = layout;
    renderFieldKeyboard();
  }, true);
  tools.append(changeLayout('lower', 'abc'), changeLayout('upper', 'ABC'),
    changeLayout('symbols', '記号'), key('空白', () => typeFieldKey(' '), true));
  tools.append(key('削除', () => typeFieldKey(''), true), key('完了', () => {
    keyboardField?.blur();
    if (panel.contains(document.activeElement)) document.activeElement.blur();
    checkInputFocus();
  }, true));
  panel.replaceChildren(...rows[keyboardLayout].map(row => el('div', {class:'field-keyboard-row'},
    ...Array.from(row, letter => key(letter, () => typeFieldKey(letter))))), tools);
}
function typeFieldKey(letter) {
  const field = keyboardField;
  if (!field) return;
  let start = field.selectionStart ?? field.value.length;
  const end = field.selectionEnd ?? start;
  if (letter && field.value.length - (end - start) + letter.length > field.maxLength) return;
  if (!letter && start === end) {
    start -= Array.from(field.value.slice(0, start)).at(-1)?.length ?? 0;
  }
  field.setRangeText(letter, start, end, 'end');
  field.dispatchEvent(new Event('input', {bubbles:true}));
}
function monitorActive() { return activeTab==='settings' && liveMonitorVisible; }
function wantsLiveReadings() { return !samplingPaused && (monitorActive() || ($('learn-dialog').open && !capturedReading)); }
function schedulePoll(failed) {
  clearTimeout(pollTimer);
  if (!csrf || document.hidden) return;
  const interval = wantsLiveReadings() ? 125 : currentStatus?.busy ? 200 : 1000;
  const delay = failed ? 500 : Math.max(10,interval-(performance.now()-pollStartedAt));
  pollTimer = setTimeout(()=>poll(false),delay);
}
async function poll(force = true) {
  if (!csrf || document.hidden) return;
  if (polling) {
    if (force) lastStatusAt = -Infinity;
    return;
  }
  clearTimeout(pollTimer);
  polling = true;
  pollStartedAt = performance.now();
  const liveOnly = !force && wantsLiveReadings() && pollStartedAt-lastStatusAt<1000;
  let failed = false;
  try {
    if (liveOnly) {
      const query = monitorActive() && chartCursor !== null ? '?after='+chartCursor : '';
      const s = await api('readings'+query);
      if (document.hidden) return;
      currentStatus = {...currentStatus, readings:s.readings, samples:s.samples, inputModePaused:s.inputModePaused};
      acceptReadings(s);
      return;
    }
    lastStatusAt = pollStartedAt;
    const includeLogs=activeTab==='settings' && $('operation-log').open;
    const params = new URLSearchParams();
    if (!includeLogs) params.set('logs','0');
    if (activeTab==='settings' && chartCursor!==null) params.set('after',chartCursor);
    const query = params.toString();
    const s=await api(query ? 'status?'+query : 'status');
    if (document.hidden) return;
    $('stop').hidden = s.busy !== true;
    samplingPaused = !s.readings && !!s.firmware;
    if (samplingPaused) {
      clearTimeout(readingTimer);
      lastReadingAt = -Infinity;
      showLiveConnection('ファームウェアの処理中');
      currentStatus = {...currentStatus, firmware:s.firmware, wifiConnected:s.wifiConnected, ble:null, bleSuspended:true, bleConnected:false, bleReady:false};
      $('connection').textContent='BLE 一時停止';
      $('connection').className='badge warn';
      renderFirmware(currentStatus);
      renderBle(currentStatus);
      return;
    }
    currentStatus=s;
    acceptReadings(s);
    setText('connection',s.ble?.enabled===false?'BLE 無効':s.bleSuspended?'BLE 一時停止':s.bleReady?'BLE 操作可能':s.bleConnected?(s.ble?.encrypted?'BLE キー通知待ち':'BLE 暗号化待ち'):'BLE 未接続');
    const connectionClass='badge '+(s.bleReady?'good':'warn');
    if ($('connection').className!==connectionClass) $('connection').className=connectionClass;
    renderInputMode();
    setText('output-state', !s.outputEnabled?'キー送信 OFF':s.inputModePaused?'キー送信 一時停止':'キー送信 ON');
    const outputClass='badge '+(s.outputEnabled&&!s.inputModePaused?'good':'warn');
    if ($('output-state').className!==outputClass) $('output-state').className=outputClass;
    if (includeLogs) renderOperationLogs(s);
    wifiStatus(s);
    renderFirmware(s);
    renderBle(s);
  } catch(error) {
    failed = true;
    clearTimeout(readingTimer);
    lastReadingAt = -Infinity;
    if (liveOnly) {
      showLiveConnection(error.status===409 ? 'ファームウェアの処理中' : '測定データの通信待ち');
      lastStatusAt = -Infinity;
    } else {
      showLiveConnection('測定データの通信待ち');
      $('stop').hidden=true;
      renderBle(currentStatus,false);
      $('connection').textContent='本体との通信待ち';
      $('connection').className='badge warn';
      $('wifi-connection-state').classList.add('stale');
      $('wifi-status-wait').hidden = false;
    }
  } finally {
    polling=false;
    if (!document.hidden) renderLearning();
    schedulePoll(failed);
  }
}
async function run(fn) { try{await fn();}catch(error){note(error.message,true);} }
$('retry-connection').addEventListener('click',connectWorkspace);
if (isTouchEditor) {
  fieldKeyboard.addEventListener('pointerdown', event => event.preventDefault());
  fieldKeyboard.addEventListener('mousedown', event => event.preventDefault());
  window.visualViewport?.addEventListener('resize', showFieldKeyboard);
  document.addEventListener('focusin', () => {
    clearTimeout(focusTimer);
    checkInputFocus();
  });
  document.addEventListener('focusout', () => {
    clearTimeout(focusTimer);
    // A tap on another field can blur the old field before the new one gains focus.
    focusTimer = setTimeout(checkInputFocus, 250);
  });
}
document.addEventListener('visibilitychange', () => {
  checkInputFocus();
  if (document.hidden) clearTimeout(pollTimer); else poll();
});
window.addEventListener('pagehide', () => {
  clearTimeout(focusTimer);
  clearTimeout(pollTimer);
  clearTimeout(readingTimer);
  updateFieldKeyboard(null);
  setInputFocus(false);
});
window.addEventListener('pageshow', () => {checkInputFocus();poll();});
$('close-learn').addEventListener('click',()=> $('learn-dialog').close());
$('macro-dialog').addEventListener('cancel',event=>event.preventDefault());
$('discard-macro').addEventListener('click',()=>{if (!saving && macroDraft) closeMacro();});
$('save-macro').addEventListener('click',saveMacro);
$('delete-macro').addEventListener('click',()=>{
  macroDeleting = !macroDeleting;
  updateMacroDeletion();
});
$('shortcut-cancel').addEventListener('click',()=>$('shortcut-dialog').close());
$('shortcut-apply').addEventListener('click',()=>{
  $('shortcut-dialog').close();
  applyShortcut(shortcutDraft);
});
$('shortcut-test').addEventListener('click',()=>testShortcut(false));
$('shortcut-delay').addEventListener('click',()=>testShortcut(true));
$('shortcut-dialog').addEventListener('close',()=>{
  if ($('shortcut-dialog').open) return;
  clearTimeout(shortcutTimer);
  shortcutDraft = applyShortcut = undefined;
  shortcutListening = false;
  checkInputFocus();
});
$('shortcut-feedback').addEventListener('keydown',event=>{
  if (!shortcutListening || event.key==='Unidentified') return;
  const isModifier = ['Control','Alt','Shift','Meta'].includes(event.key);
  if (isModifier && shortcutDraft.key!==0) return;
  const modifiers = [event.ctrlKey,event.altKey,event.shiftKey,event.metaKey];
  const keys = shortcutModifiers.filter((_,index)=>modifiers[index]).map(([,label])=>label);
  if (!isModifier) keys.push(event.key===' ' ? 'Space' : event.key.length===1 ? event.key.toUpperCase() : event.key);
  setText('shortcut-received','このページで受信: '+keys.join('＋'));
});
$('apply-learn').addEventListener('click',applyLearning);
$('learn-dialog').addEventListener('cancel',event=>{if (applyingCapture) event.preventDefault();});
$('learn-dialog').addEventListener('close',()=>{
  learningButton = capturedReading = null;
  checkInputFocus();
});
for(const button of document.querySelectorAll('nav button'))button.addEventListener('click',()=>{
  if (button.dataset.tab === 'buttons' && activeTab !== 'buttons') renderButtons();
  activeTab=button.dataset.tab;
  for(const b of document.querySelectorAll('nav button'))b.classList.toggle('selected',b===button);
  for(const section of document.querySelectorAll('[id^="tab-"]'))section.hidden=section.id!=='tab-'+activeTab;
  checkInputFocus();
  poll();
});
$('operation-log').addEventListener('toggle',()=>{if ($('operation-log').open) poll();});
$('stop').addEventListener('click',()=>run(async()=>{note((await api('stop','POST')).message);await poll();}));
$('add-button').addEventListener('click',()=>{
  if (![0,1].some(channel=>channelHasSpace(channel))) {
    note('各系統16個まで登録できます。',true);
    return;
  }
  let number = 1;
  const names = new Set(config.buttons.map(button=>button.name.trim().normalize('NFKC')));
  while (names.has('ボタン'+number)) number++;
  $('button-name-input').value = 'ボタン'+number;
  $('button-name-input').setCustomValidity('');
  $('button-name-dialog').showModal();
  $('button-name-input').select();
});
$('cancel-button-name').addEventListener('click',()=> $('button-name-dialog').close());
$('button-name-input').addEventListener('input',event=>event.target.setCustomValidity(''));
$('button-name-form').addEventListener('submit',event=>{
  event.preventDefault();
  const input = $('button-name-input');
  const name = input.value.trim();
  if (!name || config.buttons.some(button=>button.name.trim().normalize('NFKC')===name.normalize('NFKC'))) {
    input.setCustomValidity(name ? 'ほかのボタンと異なる名前を入力してください。' : 'ボタン名を入力してください。');
    input.reportValidity();
    return;
  }
  const channel = [0,1].find(index=>channelHasSpace(index));
  if (channel===undefined) return;
  const button = {id:'b'+Date.now().toString(36)+Math.random().toString(36).slice(2,6),
    name,channel,center:null,tolerance:10,shortAction:'none',doubleAction:'none',longAction:'none'};
  config.buttons.push(button);
  $('button-name-dialog').close();
  setDirty(); renderButtons();
  document.querySelector('[data-button-id="'+button.id+'"] .learn-button').focus();
});
$('add-macro').addEventListener('click',()=>openMacro({
  id:'m'+Date.now().toString(36),name:'新しい機能',steps:[{type:'wait',ms:500}],
}));
function touchWifi(){wifiDirty=true;refreshDirty();feedback('wifi-feedback','入力中：まだ保存していません。');}
for(const id of ['wifi-ssid','wifi-password','wifi-open-network'])for(const event of ['input','change'])$(id).addEventListener(event,touchWifi);
$('show-wifi-password').addEventListener('change',()=>{$('wifi-password').type=$('show-wifi-password').checked?'text':'password';});
$('wifi-form').addEventListener('submit',event=>{event.preventDefault();run(async()=>{note((await saveWifi()).message);await poll();});});
$('cancel-wifi').addEventListener('click',()=>{
  wifiDirty=false;$('wifi-ssid').value=saved.ssid;$('wifi-password').value='';
  $('wifi-open-network').checked=!!saved.ssid && currentStatus?.wifiPasswordSet === false;
  feedback('wifi-feedback','入力した変更を取り消しました。');refreshDirty();
});
$('wifi-reconnect').addEventListener('click',()=>run(async()=>{note((await api('wifi/reconnect','POST')).message);await poll();}));
$('firmware-check').addEventListener('click',()=>requestFirmwareUpdate());
$('firmware-install').addEventListener('click',()=>requestFirmwareUpdate(currentStatus?.firmware?.availableVersion));
$('copy-lan-url').addEventListener('click',()=>run(async()=>{
  const text=$('wifi-lan-url').textContent;
  if(navigator.clipboard?.writeText){try{await navigator.clipboard.writeText(text);note('PC用URLをコピーしました。');return;}catch{}}
  const range=document.createRange();range.selectNodeContents($('wifi-lan-url'));const selection=window.getSelection();selection.removeAllRanges();selection.addRange(range);
  note('URLを選択しました。長押しまたはCtrl+Cでコピーしてください。');
}));
$('backup').addEventListener('click',()=>run(async()=>{const data=await api('backup');const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'}));const link=el('a',{href:url,download:'steering-config-'+new Date().toISOString().slice(0,10)+'.json'});document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);note('保存済み設定をダウンロードしました。秘密情報は含まれません。');}));
function previewConfig(data, message) {
  config={...data, ssid:saved.ssid};
  previewingConfig=true;renderAll();note(message);
}
$('restore-file').addEventListener('change',event=>run(async()=>{
  const file=event.target.files[0];event.target.value='';if(!file)return;if(file.size>24576)throw new Error('JSONは24KB以内にしてください。');
  if(saving)throw new Error('設定の保存が終わってから読み込んでください。');
  if((dirty || previewingConfig)&&!confirm('編集中の内容を置き換えますか？'))return;
  const data=JSON.parse(await file.text());if(data.version!==1||!Array.isArray(data.channels)||data.channels.length!==2||!Array.isArray(data.buttons)||data.buttons.length>32||!Array.isArray(data.macros)||data.macros.length>16||!data.timing||!data.acc)throw new Error('設定JSONの形式が不正です。');
  data.ssid=saved.ssid;data.outputEnabled=false;
  previewConfig(await api('validate','POST',data), 'JSONを読み込みました。キー送信はOFFに設定しています。');
}));
$('load-defaults').addEventListener('click',()=>run(async()=>{
  if(saving)throw new Error('設定の保存が終わってから読み込んでください。');
  if(!confirm('編集内容を参考プリセットに置き換えますか？'))return;
  previewConfig(await api('defaults'), '参考プリセットを読み込みました。実車の値で校正してください。');
}));
$('reset-functions').addEventListener('click',()=>run(async()=>{
  if (saving || previewingConfig || !confirm('標準機能の名前と手順を初期値に戻しますか？')) return;
  saving=true;refreshDirty();
  try {
    const result = await api('functions/defaults','POST');
    for (const key of ['functions','macros']) {
      config[key] = result[key]; saved[key] = clone(result[key]);
    }
    renderMacros();renderButtons();poll();
    note('機能のプリセットを初期値に戻しました。');
  } finally { saving=false;refreshDirty(); }
}));
$('save-preview').addEventListener('click',()=>run(async()=>{
  note((await saveSettings(configKeys, true)).message);
  previewingConfig=false;refreshDirty();
}));
$('cancel-preview').addEventListener('click',()=>{
  config=clone(saved);previewingConfig=false;renderAll();
});
$('reboot').addEventListener('click',()=>run(async()=>{if(confirm('本体を再起動しますか？未保存の変更は失われます。'))note((await api('reboot','POST')).message);}));
$('reconnect-ble').addEventListener('click',()=>run(async()=>{note((await api('ble/reconnect','POST')).message);await poll();}));
$('forget-ble').addEventListener('click',()=>run(async()=>{if(confirm('BLEペアリング情報を削除して再起動しますか？接続先の端末側でもデバイス登録を解除してください。'))note((await api('ble/forget','POST')).message);}));
$('reset').addEventListener('click',()=>run(async()=>{if(!confirm('入力・Wi-Fi・端末パスコードを初期化しますか？設定用APのパスワードとBLEペアリングは維持します。'))return;const result=await api('reset','POST',{confirm:'RESET'});note(result.message);await openWorkspace();}));
window.addEventListener('beforeunload',event=>{if(hasChanges()){event.preventDefault();event.returnValue='';}});
if ('IntersectionObserver' in window) {
  new IntersectionObserver(([entry])=>{
    liveMonitorVisible=entry.isIntersecting;
    if (liveMonitorVisible) poll(false);
  }).observe($('live-cards'));
}
window.addEventListener('resize',()=>{if (liveMonitorVisible && !document.hidden) for(let i=0;i<2;i++)drawChart(i);});
connectWorkspace();
