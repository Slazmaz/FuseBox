#ifndef FUSEBOX_STEAMUI_JS_H
#define FUSEBOX_STEAMUI_JS_H

/* Run in Steam's SharedJSContext. Returns "ready", "wait" (Steam is still
 * starting) or "unsupported".
 *
 * Steam keeps what it knows of achievements in a query cache, three entries
 * a game: its achievements, how many players have each, and which the user
 * has. A shortcut has none, so they're put there, dated years ahead so
 * Steam never asks its servers instead (and put back if it did); and a
 * game page's sections get the one for achievements, which Steam leaves out
 * for shortcuts. The user's is by account: while Steam logs in that's one
 * ending in number 0 (76561197960265728), which is nobody's.
 *
 * Modules are found by what's in them: their numbers change with every
 * Steam update. */
#define STEAMUI_JS \
   "(() => {\n" \
   "   const me = window.App && App.m_CurrentUser && App.m_CurrentUser.strSteamID;\n" \
   "   if (!me || BigInt(me) % 4294967296n === 0n || !window.webpackChunksteamui || !window.appStore\n" \
   "         || !window.SteamUIStore || !SteamUIStore.WindowStore\n" \
   "         || !SteamUIStore.WindowStore.SteamUIWindows.length) return 'wait';\n" \
   "   if (window.__fusebox) {\n" \
   "      if (window.__fusebox.me === me && window.__fusebox.version === 12) return 'ready';\n" \
   "      window.__fusebox.undo();\n" \
   "   }\n" \
   "   let req, page, cache, keys = false;\n" \
   "   webpackChunksteamui.push([[Symbol('fusebox')], {}, r => { req = r; }]);\n" \
   "   if (!req || !req.m) return 'unsupported';\n" \
   "   const pick = (id, test) => {\n" \
   "      const exports = req(id);\n" \
   "      return Object.values(exports).find(x => { try { return x && test(x); } catch (e) { return false; } });\n" \
   "   };\n" \
   "   for (const id of Object.keys(req.m)) {\n" \
   "      const src = String(req.m[id]);\n" \
   "      if (!page && src.includes('GetSections(') && src.includes('\"nonsteam\"'))\n" \
   "         page = pick(id, x => x.prototype && x.prototype.GetSections);\n" \
   "      if (!cache && /new \\w+\\.\\w+\\(\\{defaultOptions/.test(src))\n" \
   "         cache = pick(id, x => x.setQueryData && x.getQueryCache);\n" \
   "      if (src.includes('\"globalpercentages\"') && src.includes('\"user_achievements\"'))\n" \
   "         keys = true;\n" \
   "   }\n" \
   "   if (!page || !cache || !keys) return 'unsupported';\n" \
   "   const games = new Map(), original = page.prototype.GetSections;\n" \
   "   /* The pages drawn last, to draw again when their sections change. */\n" \
   "   let pages = [];\n" \
   "   const mark = app => setTimeout(() => pages.forEach(p => {\n" \
   "      try { if (p.props.overview.appid === app) p.forceUpdate(); } catch (e) { }\n" \
   "   }), 0);\n" \
   "   page.prototype.GetSections = function(overview) {\n" \
   "      const sections = original.apply(this, arguments);\n" \
   "      if (!pages.includes(this)) pages = [this, ...pages].slice(0, 4);\n" \
   "      if (!overview || !games.has(overview.appid)) return sections;\n" \
   "      return new Set(sections).add('achievements');\n" \
   "   };\n" \
   "   const names = app => [['achievements', app, 'schema'], ['achievements', app, 'globalpercentages'],\n" \
   "      ['achievements', app, 'user_achievements', me]];\n" \
   "   const drop = app => {\n" \
   "      games.delete(app);\n" \
   "      mark(app);\n" \
   "      names(app).forEach(queryKey => cache.removeQueries({ queryKey }));\n" \
   "   };\n" \
   "   const put = (app, list) => {\n" \
   "      const at = { updatedAt: Date.now() + 10 * 365 * 864e5 };\n" \
   "      const badge = 'https://media.retroachievements.org/Badge/';\n" \
   "      const key = names(app), got = list.filter(a => a[5]);\n" \
   "      cache.setQueryDefaults(['achievements', app], { gcTime: Infinity });\n" \
   "      cache.setQueryData(key[0], { appid: app, language: 'english', schema_hash: 1, schema_version: 1,\n" \
   "         groups: [{ id: 0, name: '', archived: false, ispublic: true, developeronly: false,\n" \
   "            dlcappid: 0, order: 0, achievements: list.map(a => ({ internal_key: a[0],\n" \
   "               api_name: 'RA_' + a[0], name: a[1], description: a[2], hidden: false, archived: false,\n" \
   "               icon_achieved: badge + a[3] + '.png', icon_unachieved: badge + a[3] + '_lock.png',\n" \
   "               groupid: 0 })) }] }, at);\n" \
   "      cache.setQueryData(key[1], { percentages: Object.fromEntries(list.map(a => [a[0], a[4]])) }, at);\n" \
   "      cache.setQueryData(key[2], { appid: app, steamid: me, schema_hash: 1,\n" \
   "         achievements: Object.fromEntries(got.map(a => [a[0], { internal_key: a[0], unlocked: true,\n" \
   "            unlock_time: 0 }])),\n" \
   "         groups: { 0: { groupid: 0, is_achievable: true, completed_achievements: got.length,\n" \
   "            is_completed: got.length === list.length } } }, at);\n" \
   "   };\n" \
   "   const ours = q => q && q.state.dataUpdatedAt > Date.now() + 864e5;\n" \
   "   const stop = cache.getQueryCache().subscribe(e => {\n" \
   "      const key = e.query && e.query.queryKey, list = key && games.get(key[1]);\n" \
   "      if (!list || key[0] !== 'achievements' || e.type !== 'updated' || ours(e.query)\n" \
   "            || !e.action || e.action.type !== 'success') return;\n" \
   "      if (key[2] === 'schema' || key[2] === 'globalpercentages'\n" \
   "            || (key[2] === 'user_achievements' && key[3] === me))\n" \
   "         setTimeout(() => games.has(key[1]) && put(key[1], games.get(key[1])), 0);\n" \
   "   });\n" \
   "   window.__fusebox = {\n" \
   "      me,\n" \
   "      version: 12,\n" \
   "      set(app, list) {\n" \
   "         const first = !games.has(app);\n" \
   "         games.set(app, list);\n" \
   "         put(app, list);\n" \
   "         if (first) mark(app);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      keep(apps) {\n" \
   "         [...games.keys()].filter(a => !apps.includes(a)).forEach(drop);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      undo() {\n" \
   "         stop();\n" \
   "         [...games.keys()].forEach(drop);\n" \
   "         page.prototype.GetSections = original;\n" \
   "         delete window.__fusebox;\n" \
   "         return 'ok';\n" \
   "      }\n" \
   "   };\n" \
   "   return 'ready';\n" \
   "})()"

#endif
