// Care circle page — a Mini Program port of the web client's care-circle modal
// (htdoc/src/modals.js showManageCircleModal), backed by /api/circle/*:
// list my circles + members, create, invite by email, accept/decline
// invitations, remove members, delete a circle, rename (owner), edit member
// nicknames (admins), promote/demote members (owner), and set my own
// health-data sharing level per circle. `<select>` has no Mini Program
// equivalent, so role / health level use <picker mode="selector">.
var api = require('../../utils/api.js');
var auth = require('../../utils/auth.js');

// Role picker options; the key is what /api/circle/role expects.
var ROLE_OPTS = [
  { key: 'member', name: '成员' },
  { key: 'maintainer', name: '管理员' },
];
// Health-sharing picker options; the key is what /api/circle/health-sharing
// expects. Index maps to my_health_access: 0 off, 1 view, 2 edit.
var HEALTH_OPTS = [
  { key: 'off', name: '不共享' },
  { key: 'view', name: '仅查看' },
  { key: 'edit', name: '可编辑' },
];

function roleLabel(role) {
  if (role === 2) { return '所有者'; }
  if (role === 1) { return '管理员'; }
  return '成员';
}

Page({
  data: {
    loading: true,
    error: false,
    circles: [],
    invites: [],
    newName: '',
    roleNames: ROLE_OPTS.map(function (o) { return o.name; }),
    healthNames: HEALTH_OPTS.map(function (o) { return o.name; }),
  },

  onShow: function () {
    if (!auth.getToken()) {
      wx.reLaunch({ url: '/pages/login/login' });
      return;
    }
    this.load();
  },

  // GET /api/circle/members -> { circles:[{ circle_id, name, my_role,
  // my_health_access, members[] }], invites:[{ token, owner_email, circle_name }] }.
  // Display fields (labels, badges, permission flags, picker indexes) are
  // precomputed here so the WXML stays declarative.
  load: function () {
    var self = this;
    this.setData({ loading: true, error: false });
    api.get('/api/circle/members')
      .then(function (data) {
        var circles = (data && data.circles instanceof Array) ? data.circles : [];
        var invites = (data && data.invites instanceof Array) ? data.invites : [];
        self.setData({
          loading: false,
          circles: circles.map(function (c) { return self.decorateCircle(c); }),
          invites: invites.map(function (inv) {
            var label = (inv.circle_name && inv.owner_email)
              ? (inv.owner_email + ' · ' + inv.circle_name)
              : (inv.owner_email || inv.circle_name || '邀请');
            return { token: inv.token, label: label };
          }),
        });
      })
      .catch(function (err) {
        if (err && err.code === 401) { wx.reLaunch({ url: '/pages/login/login' }); return; }
        self.setData({ loading: false, error: true });
      });
  },

  decorateCircle: function (c) {
    var isOwner = c.my_role === 2;
    var isAdmin = c.my_role >= 1;
    var lvl = c.my_health_access || 0;
    var members = (c.members instanceof Array) ? c.members : [];
    return {
      circle_id: c.circle_id,
      name: c.name || '未命名关怀圈',
      myRoleLabel: roleLabel(c.my_role),
      isOwner: isOwner,
      isAdmin: isAdmin,
      inviteEmail: '',
      healthIndex: lvl >= 2 ? 2 : (lvl >= 1 ? 1 : 0),
      healthKey: lvl >= 2 ? 'edit' : (lvl >= 1 ? 'view' : 'off'),
      members: members.map(function (m) {
        var isOwnerMember = m.role === 2;
        var badges = [];
        if (m.me) { badges.push('你'); }
        if (m.status && m.status !== 'accepted') { badges.push('待接受'); }
        return {
          member: m.member,
          email: m.email || '',
          nickname: m.nickname || '',
          name: m.nickname || m.email || ('#' + m.member),
          badges: badges,
          ownerBadge: isOwnerMember,
          // A fellow maintainer, shown as a static badge only when the owner
          // isn't editing roles here.
          maintainerBadge: !isOwner && !isOwnerMember && m.role === 1,
          nickEditable: isAdmin && !isOwnerMember,
          roleEditable: isOwner && !isOwnerMember,
          roleIndex: m.role === 1 ? 1 : 0,
          roleKey: m.role === 1 ? 'maintainer' : 'member',
          // Owner removes any non-owner; a maintainer removes plain members only.
          canRemove: (isOwner && !isOwnerMember) || (c.my_role === 1 && m.role === 0 && !m.me),
        };
      }),
    };
  },

  // Shared handler for the fire-and-reload mutations.
  mutate: function (uri, body, failMsg) {
    var self = this;
    api.post(uri, body)
      .then(function () { self.load(); })
      .catch(function (err) {
        if (err && err.code === 401) { wx.reLaunch({ url: '/pages/login/login' }); return; }
        wx.showToast({ title: (err && err.message) || failMsg, icon: 'none' });
      });
  },

  // -- inputs -------------------------------------------------------------
  onNewNameInput: function (e) {
    this.setData({ newName: e.detail.value });
  },

  onInviteInput: function (e) {
    var idx = e.currentTarget.dataset.idx;
    this.setData({ ['circles[' + idx + '].inviteEmail']: e.detail.value });
  },

  // -- create / invite / membership --------------------------------------
  onCreate: function () {
    var name = (this.data.newName || '').trim();
    if (!name) { wx.showToast({ title: '请输入名称', icon: 'none' }); return; }
    this.setData({ newName: '' });
    this.mutate('/api/circle/create', { name: name }, '创建失败');
  },

  onInvite: function (e) {
    var idx = e.currentTarget.dataset.idx;
    var c = this.data.circles[idx];
    if (!c) { return; }
    var email = (c.inviteEmail || '').trim();
    if (!email) { wx.showToast({ title: '请输入邮箱', icon: 'none' }); return; }
    this.mutate('/api/circle/invite', { email: email, care_circle_id: c.circle_id }, '邀请失败');
  },

  onRemove: function (e) {
    var self = this;
    var ds = e.currentTarget.dataset;
    wx.showModal({
      title: '移除成员',
      content: '确定将该成员移出关怀圈吗？',
      confirmColor: '#ba1a1a',
      success: function (r) {
        if (r.confirm) {
          self.mutate('/api/circle/remove', { member: ds.member, care_circle_id: ds.circle }, '移除失败');
        }
      },
    });
  },

  onDelete: function (e) {
    var self = this;
    var ds = e.currentTarget.dataset;
    wx.showModal({
      title: '解散关怀圈',
      content: '确定解散「' + (ds.name || '') + '」吗？此操作不可撤销。',
      confirmColor: '#ba1a1a',
      success: function (r) {
        if (r.confirm) {
          self.mutate('/api/circle/delete', { care_circle_id: ds.circle }, '解散失败');
        }
      },
    });
  },

  onAccept: function (e) {
    this.mutate('/api/circle/accept', { token: e.currentTarget.dataset.token }, '操作失败');
  },

  onDecline: function (e) {
    this.mutate('/api/circle/decline', { token: e.currentTarget.dataset.token }, '操作失败');
  },

  // -- rename / nickname / role / health (only re-post when changed) ------
  onRename: function (e) {
    var ds = e.currentTarget.dataset;
    var name = (e.detail.value || '').trim();
    if (!name || name === ds.old) { return; }
    this.mutate('/api/circle/rename', { care_circle_id: ds.circle, name: name }, '重命名失败');
  },

  onNickname: function (e) {
    var ds = e.currentTarget.dataset;
    var nickname = (e.detail.value || '').trim();
    if (nickname === (ds.old || '')) { return; }   // unchanged (incl. clearing an empty one)
    this.mutate('/api/circle/nickname',
                { care_circle_id: ds.circle, member: ds.member, nickname: nickname }, '保存失败');
  },

  onRole: function (e) {
    var ds = e.currentTarget.dataset;
    var opt = ROLE_OPTS[Number(e.detail.value) || 0];
    if (!opt || opt.key === ds.current) { return; }
    this.mutate('/api/circle/role',
                { care_circle_id: ds.circle, member: ds.member, role: opt.key }, '修改失败');
  },

  onHealth: function (e) {
    var ds = e.currentTarget.dataset;
    var opt = HEALTH_OPTS[Number(e.detail.value) || 0];
    if (!opt || opt.key === ds.current) { return; }
    this.mutate('/api/circle/health-sharing',
                { access: opt.key, care_circle_id: ds.circle }, '修改失败');
  },
});
