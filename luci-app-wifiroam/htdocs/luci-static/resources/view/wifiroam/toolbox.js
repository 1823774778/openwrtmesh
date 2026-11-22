/*
 * wifiroam Toolbox - config backup, Wi-Fi schedule, DHCP reservations, health score.
 * All actions call the real ubus methods provided by wifiroamd.
 */
'use strict';
'require baseclass';
'require rpc';
'require ui';
'require wifiroam.common as wr';

return baseclass.extend({
    title: function () { return _('Toolbox'); },

    load: function () {
        return Promise.all([
            wr.safeCall('status'),
            wr.safeCall('health_score'),
            wr.safeCall('wifi_schedule_get'),
            wr.safeCall('dhcp_static_list'),
        ]);
    },

    render: function (st, health, sched, dhcp) {
        var self = this;
        this.st = st || {};
        var isCap = this.st.role === 'cap';

        var body = [wr.css(), wr.appHeader('toolbox')];

        if (!this.st.role) {
            body.push(E('div', { class: 'alert-message danger' },
                _('Cannot reach wifiroamd. Start the service on the Wireless Settings page, then reload this page.')));
            return E('div', { class: 'wr-app' }, body);
        }

        // ---- Health score ----
        var h = health || {};
        var score = h.score != null ? h.score : 0;
        var grade = h.grade || 'poor';
        var gradeText = {
            excellent: _('Excellent'), good: _('Good'), fair: _('Fair'), poor: _('Poor')
        }[grade] || grade;
        var ringColor = { excellent: '#35c98a', good: '#4f9dff', fair: '#ffb547', poor: '#ff6b6b' }[grade] || '#888';
        var scoreRing = E('div', { class: 'wr-hc-ring', style: 'border-color:' + ringColor }, [
            E('span', { class: 'wr-hc-num' }, '' + score),
            E('span', { class: 'wr-hc-unit' }, '/ 100'),
        ]);
        var checks = E('div', { class: 'wr-hc-checks' });
        (h.checks || []).forEach(function (c) {
            var icon = { ok: '✓', warn: '!', fail: '✕', info: 'i' }[c.status] || 'i';
            var cls = 'wr-hc-item wr-hc-' + (c.status || 'info');
            checks.appendChild(E('div', { class: cls }, [
                E('span', { class: 'wr-hc-ico' }, icon),
                E('span', { class: 'wr-hc-label' }, c.label || ''),
                E('span', { class: 'wr-hc-detail' }, c.detail || ''),
            ]));
        });
        var healthPanel = E('div', { class: 'cbi-section' }, [
            E('h4', {}, _('Network Health')),
            E('div', { class: 'wr-health-row' }, [
                scoreRing,
                E('div', { class: 'wr-health-meta' }, [
                    E('div', { class: 'wr-hc-grade', style: 'color:' + ringColor }, gradeText),
                    E('div', { class: 'wr-hint' }, _('Computed from node status, roaming config and known devices.')),
                ]),
            ]),
            checks,
            E('div', { class: 'wr-toolbar' }, [
                E('button', {
                    class: 'btn',
                    click: function () { self.refresh(); },
                }, _('Refresh')),
            ]),
        ]);

        // ---- Wi-Fi schedule ----
        var s = sched || {};
        var enCb = E('input', { type: 'checkbox', class: 'wr-switch-input' });
        enCb.checked = !!s.enabled;
        var wdWrap = E('div', { class: 'wr-wd-picker' });
        var WD = [ [1, _('Mon')], [2, _('Tue')], [3, _('Wed')], [4, _('Thu')],
                   [5, _('Fri')], [6, _('Sat')], [7, _('Sun')] ];
        var dayCbs = {};
        WD.forEach(function (d) {
            var cb = E('input', { type: 'checkbox' });
            cb.checked = ((s.weekdays || []).indexOf(d[0]) >= 0);
            dayCbs[d[0]] = cb;
            wdWrap.appendChild(E('label', { class: 'wr-wd-label' }, [cb, ' ' + d[1]]));
        });
        var startIn = E('input', { type: 'time', class: 'cbi-input-text', value: s.start || '08:00' });
        var endIn = E('input', { type: 'time', class: 'cbi-input-text', value: s.end || '23:00' });
        var schedStatus = E('span', { class: 'wr-hint' },
            _('Current state') + ': ' + (s.open ? _('On') : _('Off')));
        var applyBtn = E('button', { class: 'btn cbi-button cbi-button-action' }, _('Save & Apply'));
        applyBtn.addEventListener('click', function () {
            var days = [];
            for (var k in dayCbs) if (dayCbs[k].checked) days.push(parseInt(k));
            self.setBusy(applyBtn, true);
            wr.call('wifi_schedule_set', {
                enabled: enCb.checked,
                start: startIn.value,
                end: endIn.value,
                weekdays: days.join(' '),
            }).then(function (res) {
                self.setBusy(applyBtn, false);
                if (res && res.ok) {
                    wr.notify('success', _('Applied'));
                    self.refresh();
                } else {
                    wr.notify('danger', _('Failed') + ': ' + ((res && res.error) || _('unknown')));
                }
            }).catch(function () {
                self.setBusy(applyBtn, false);
                wr.notify('danger', _('request rejected'));
            });
        });
        var schedPanel = E('div', { class: 'cbi-section' }, [
            E('h4', {}, _('Wi-Fi Schedule')),
            E('div', { class: 'wr-form-row' }, [
                E('label', {}, _('Enable scheduled on/off')), enCb,
            ]),
            E('div', { class: 'wr-form-row' }, [
                E('label', {}, _('On time')), startIn,
            ]),
            E('div', { class: 'wr-form-row' }, [
                E('label', {}, _('Off time')), endIn,
            ]),
            E('div', { class: 'wr-form-row' }, [
                E('label', {}, _('Active days')), wdWrap,
            ]),
            schedStatus,
            E('div', { class: 'wr-toolbar' }, [ applyBtn ]),
            E('div', { class: 'wr-hint' },
                _('Outside the active window the main Wi-Fi SSIDs are disabled until the next window.')),
        ]);
        if (!isCap) self.disablePanel(schedPanel);

        // ---- DHCP static leases ----
        var listWrap = E('div', { class: 'wr-dhcp-list' });
        function renderDhcp(d) {
            listWrap.textContent = '';
            var rows = d && d.reservations || [];
            if (!rows.length) {
                listWrap.appendChild(E('div', { class: 'wr-empty' }, _('No static leases yet.')));
                return;
            }
            rows.forEach(function (r) {
                listWrap.appendChild(E('div', { class: 'wr-dhcp-row' }, [
                    E('span', { class: 'wr-dhcp-mac' }, r.mac),
                    E('span', { class: 'wr-dhcp-ip' }, r.ip),
                    E('span', { class: 'wr-dhcp-name' }, r.name || '-'),
                    E('button', {
                        class: 'btn',
                        click: function () {
                            wr.call('dhcp_static_del', { mac: r.mac }).then(function (res) {
                                if (res && res.ok) { wr.notify('success', _('Removed')); self.refresh(); }
                                else wr.notify('danger', _('Failed'));
                            }).catch(function () { wr.notify('danger', _('request rejected')); });
                        },
                    }, _('Delete')),
                ]));
            });
        }
        renderDhcp(dhcp);
        var macIn = E('input', { type: 'text', class: 'cbi-input-text', placeholder: 'AA:BB:CC:DD:EE:FF' });
        var ipIn = E('input', { type: 'text', class: 'cbi-input-text', placeholder: '192.168.1.50' });
        var nameIn = E('input', { type: 'text', class: 'cbi-input-text', placeholder: _('Name') });
        var addBtn = E('button', { class: 'btn cbi-button cbi-button-action' }, _('Add reservation'));
        addBtn.addEventListener('click', function () {
            self.setBusy(addBtn, true);
            wr.call('dhcp_static_set', {
                mac: macIn.value, ip: ipIn.value, name: nameIn.value,
            }).then(function (res) {
                self.setBusy(addBtn, false);
                if (res && res.ok) {
                    wr.notify('success', _('Added'));
                    macIn.value = ''; ipIn.value = ''; nameIn.value = '';
                    self.refresh();
                } else {
                    wr.notify('danger', _('Failed') + ': ' + ((res && res.error) || _('unknown')));
                }
            }).catch(function () {
                self.setBusy(addBtn, false);
                wr.notify('danger', _('request rejected'));
            });
        });
        var dhcpPanel = E('div', { class: 'cbi-section' }, [
            E('h4', {}, _('DHCP Static Leases')),
            listWrap,
            E('div', { class: 'wr-dhcp-add' }, [ macIn, ipIn, nameIn, addBtn ]),
            E('div', { class: 'wr-hint' },
                _('Reservations assign a fixed IP to a device. dnsmasq restarts after each change.')),
        ]);
        if (!isCap) self.disablePanel(dhcpPanel);

        // ---- Config backup / restore ----
        var exportBtn = E('button', { class: 'btn' }, _('Download backup'));
        exportBtn.addEventListener('click', function () {
            self.setBusy(exportBtn, true);
            wr.call('config_export', {}).then(function (res) {
                self.setBusy(exportBtn, false);
                if (!res) { wr.notify('danger', _('Failed')); return; }
                var blob = new Blob([ JSON.stringify(res, null, 2) ], { type: 'application/json' });
                var a = E('a', { href: URL.createObjectURL(blob),
                    download: 'wifiroam-backup-' + new Date().toISOString().slice(0, 10) + '.json' });
                document.body.appendChild(a); a.click(); document.body.removeChild(a);
                setTimeout(function () { URL.revokeObjectURL(a.href); }, 1000);
                wr.notify('success', _('Backup downloaded'));
            }).catch(function () {
                self.setBusy(exportBtn, false);
                wr.notify('danger', _('request rejected'));
            });
        });
        var fileIn = E('input', { type: 'file', accept: '.json,application/json' });
        var importBtn = E('button', { class: 'btn cbi-button cbi-button-action' }, _('Restore backup'));
        importBtn.addEventListener('click', function () {
            if (!fileIn.files || !fileIn.files.length) {
                wr.notify('warning', _('Choose a backup file first'));
                return;
            }
            var reader = new FileReader();
            reader.onload = function () {
                self.setBusy(importBtn, true);
                wr.call('config_import', { data: String(reader.result) }).then(function (res) {
                    self.setBusy(importBtn, false);
                    if (res && res.ok) {
                        wr.notify('success', _('Restored. The page will reload.'));
                        setTimeout(function () { location.reload(); }, 1200);
                    } else {
                        wr.notify('danger', _('Failed') + ': ' + ((res && res.error) || _('unknown')));
                    }
                }).catch(function () {
                    self.setBusy(importBtn, false);
                    wr.notify('danger', _('request rejected'));
                });
            };
            reader.readAsText(fileIn.files[0]);
        });
        var backupPanel = E('div', { class: 'cbi-section' }, [
            E('h4', {}, _('Configuration Backup')),
            E('div', { class: 'wr-toolbar' }, [ exportBtn ]),
            E('div', { class: 'wr-dhcp-add' }, [ fileIn, importBtn ]),
            E('div', { class: 'wr-hint' },
                _('Backup contains all wifiroam settings, device aliases and history. Restore overwrites the current configuration.')),
        ]);
        if (!isCap) self.disablePanel(backupPanel);

        body.push(healthPanel, schedPanel, dhcpPanel, backupPanel);
        return E('div', { class: 'wr-app' }, body);
    },

    disablePanel: function (panel) {
        panel.querySelectorAll('input, button, select').forEach(function (el) {
            el.setAttribute('disabled', 'disabled');
        });
    },

    setBusy: function (el, on) { return wr.setBusy(el, on); },

    refresh: function () {
        // 重新加载整页，确保四个面板都从守护进程取到最新状态。
        location.reload();
        return Promise.resolve();
    },

    handleSaveApply: null,
    handleSave: null,
    handleReset: null,
});
