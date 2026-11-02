'use strict';
'require view';
'require rpc';
'require ui';

// The info screen's apps (plugins): the list, installing a package uploaded
// from this browser, uninstalling.  rpcd object e5-infoscreen
// (/usr/share/rpcd/ucode/e5-infoscreen.uc).
var UPLOAD = '/tmp/e5-plugin.upload';
var UPDATE_UPLOAD = '/tmp/e5-infoscreen.update.upload';

var callList = rpc.declare({ object: 'e5-infoscreen', method: 'list', expect: { apps: [] } });
var callVersion = rpc.declare({ object: 'e5-infoscreen', method: 'version', expect: { version: '' } });
var callInstall = rpc.declare({ object: 'e5-infoscreen', method: 'install' });
var callRemove = rpc.declare({ object: 'e5-infoscreen', method: 'remove', params: [ 'id' ] });
var callUpdateInstall = rpc.declare({ object: 'e5-infoscreen', method: 'update_install' });

function result(r, done) {
	if (r && r.ok) {
		ui.addNotification(null, E('p', done + (r.message ? '：' + r.message : '')), 'info');
		window.setTimeout(function() { location.reload(); }, 800);
	} else {
		ui.addNotification(null, E('p', '失败：' + ((r && r.message) || '?')), 'danger');
	}
}

return view.extend({
	handleUpload: function() {
		return ui.uploadFile(UPLOAD).then(function() {
			return callInstall();
		}).then(function(r) {
			result(r, '已安装');
		}).catch(function(e) {
			if (e && e.message) ui.addNotification(null, E('p', e.message), 'danger');
		});
	},

	handleRemove: function(app) {
		if (!confirm('卸载“' + (app.name || app.id) + '”？' + (app.builtin ? '（内置应用会在下次更新系统镜像时回来）' : '')))
			return;
		return callRemove(app.id).then(function(r) { result(r, '已卸载'); });
	},

	handleUpdateUpload: function() {
		return ui.uploadFile(UPDATE_UPLOAD).then(function() {
			return callUpdateInstall();
		}).then(function(r) {
			result(r, '信息屏已更新，正在重启');
		}).catch(function(e) {
			if (e && e.message) ui.addNotification(null, E('p', e.message), 'danger');
		});
	},

	render: function(data) {
		var apps = data.apps || [], version = data.version || '-';
		var rows = (apps || []).map(L.bind(function(a) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, a.name || a.id),
				E('td', { 'class': 'td' }, E('code', a.id)),
				E('td', { 'class': 'td' }, a.version || '-'),
				E('td', { 'class': 'td' }, a.builtin ? '内置' : '已安装'),
				E('td', { 'class': 'td cbi-section-actions' }, E('button', {
					'class': 'btn cbi-button cbi-button-remove',
					'click': ui.createHandlerFn(this, 'handleRemove', a)
				}, '卸载'))
			]);
		}, this));

		return E([], [
			E('h2', '信息屏应用'),
			E('div', { 'class': 'cbi-map-descr' },
				'设备屏幕上“应用”页里的应用（插件）。应用包是一个 .tar.gz 或 .zip 压缩包，根目录（或唯一的顶层目录）里有 manifest.json；' +
				'格式见 e5-infoscreen 的 docs/API.md。应用可以带后端代码并以 root 身份运行，只安装你信任、看过代码的应用。'),
			E('div', { 'class': 'cbi-section' }, [
				E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr table-titles' }, [
						E('th', { 'class': 'th' }, '名称'), E('th', { 'class': 'th' }, 'ID'),
						E('th', { 'class': 'th' }, '版本'), E('th', { 'class': 'th' }, '来源'),
						E('th', { 'class': 'th cbi-section-actions' }, '')
					])
				].concat(rows.length ? rows : [ E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, '没有应用')) ]))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', '安装应用'),
				E('p', '选择应用包上传并安装；同一 ID 的应用会被替换（即升级）。安装的应用保存在 /etc/e5-infoscreen/plugins，更新系统镜像后仍在。'),
				E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, 'handleUpload') }, '上传并安装…')
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', '更新信息屏'),
				E('p', '当前版本：' + version + '。上传由本项目 CI 生成的 e5-infoscreen-*.tar.gz；安装前会检查归档路径、版本和大小，设置与已安装应用会保留。'),
				E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, 'handleUpdateUpload') }, '上传并更新…')
			])
		]);
	},

	load: function() {
		return Promise.all([ callList(), callVersion() ]).then(function(r) {
			return { apps: r[0] || [], version: r[1].version || '-' };
		});
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
