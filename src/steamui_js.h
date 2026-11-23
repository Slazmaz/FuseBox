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
 * The friends section is a query too: who plays a game, by account. Friends
 * on RetroAchievements have none, so they get numbers no account has, and
 * Steam's friend store answers for those with people made here. What these
 * need may be missing after an update: achievements do without. It isn't
 * ready yet when the account is: they're given once it is. Modules are
 * found by what's in them: their numbers change with every Steam update. */
#define STEAMUI_JS \
   "(() => {\n" \
   "   const me = window.App && App.m_CurrentUser && App.m_CurrentUser.strSteamID;\n" \
   "   if (!me || BigInt(me) % 4294967296n === 0n || !window.webpackChunksteamui || !window.appStore\n" \
   "         || !window.SteamUIStore || !SteamUIStore.WindowStore\n" \
   "         || !SteamUIStore.WindowStore.SteamUIWindows.length) return 'wait';\n" \
   "   if (window.__fusebox) {\n" \
   "      if (window.__fusebox.me === me && window.__fusebox.version === 13) return 'ready';\n" \
   "      window.__fusebox.undo();\n" \
   "   }\n" \
   "   let req, page, cache, store, links, plays = false, keys = false;\n" \
   "   let react, hover, steamCard, css, strips;\n" \
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
   "      if (!store && src.includes('GetFriendsInGame(') && src.includes('m_mapPlayerCache'))\n" \
   "         store = pick(id, x => x.GetFriendState && x.GetFriendsInGame);\n" \
   "      if (!links && /BuildSteamURL\\(\\w+,\\.\\.\\.\\w+\\)\\{/.test(src))\n" \
   "         links = pick(id, x => typeof x === 'object' && x.BuildSteamURL);\n" \
   "      if (src.includes('\"FriendsGameplayInfo\"')) plays = true;\n" \
   "      /* For the card over a friend. */\n" \
   "      try {\n" \
   "         if (!hover && src.includes('RenderMiniProfile') && src.includes('renderPlaytime'))\n" \
   "            hover = pick(id, x => x.prototype && Object.hasOwn(x.prototype, 'RenderMiniProfile'));\n" \
   "         if (!steamCard && src.includes('bottom_section_additional:'))\n" \
   "            steamCard = pick(id, x => x.prototype && x.prototype.render && /bottom_section_additional:/.test(x));\n" \
   "         if (!css && src.includes('miniProfileGameContainer:')) {\n" \
   "            const x = req(id);\n" \
   "            css = x.miniProfileGameContainer ? x : pick(id, y => y.miniProfileGameContainer);\n" \
   "         }\n" \
   "         if (!strips && src.includes('PlaytimeSection:') && src.includes('PlaytimeContent:')) {\n" \
   "            const x = req(id);\n" \
   "            strips = x.PlaytimeSection ? x : pick(id, y => y.PlaytimeSection);\n" \
   "         }\n" \
   "         if (!react && src.includes('cloneElement') && src.includes('useSyncExternalStore')) {\n" \
   "            const x = req(id);\n" \
   "            if (x.createElement && x.Component) react = x;\n" \
   "         }\n" \
   "      } catch (e) { }\n" \
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
   "      const more = new Set(sections).add('achievements');\n" \
   "      return players.has(overview.appid) ? more.add('friends') : more;\n" \
   "   };\n" \
   "   const names = app => [['achievements', app, 'schema'], ['achievements', app, 'globalpercentages'],\n" \
   "      ['achievements', app, 'user_achievements', me]];\n" \
   "   /* Friends: by name, and by the number they go by here; each game's. */\n" \
   "   const people = new Map(), numbers = new Map(), players = new Map(), later = new Map();\n" \
   "   let Friend, state, inGame, undoFriends = () => {};\n" \
   "   const FIRST = 0xffff0000;\n" \
   "   const id64 = n => (76561197960265728n + BigInt(n)).toString();\n" \
   "   const playing = app => ['FriendsGameplayInfo', me, app, true];\n" \
   "   const ready = () => {\n" \
   "      if (Friend || !store || !links || !plays) return !!Friend;\n" \
   "      try { Friend = store.GetFriendState(store.currentUserSteamID).constructor; } catch (e) { return false; }\n" \
   "      state = store.GetFriendState;\n" \
   "      inGame = store.GetFriendsInGame;\n" \
   "      const count = store.GetCountFriendsInGame;\n" \
   "      const link = links.BuildSteamURL;\n" \
   "      store.GetFriendState = function(who) {\n" \
   "         const n = typeof who === 'number' ? who : who && who.GetAccountID && who.GetAccountID();\n" \
   "         return numbers.has(n) ? numbers.get(n).friend : state.apply(this, arguments);\n" \
   "      };\n" \
   "      store.GetFriendsInGame = function(app) {\n" \
   "         /* Asked either way: so what draws from it is drawn again (poke). */\n" \
   "         const real = inGame.apply(this, arguments);\n" \
   "         if (!players.has(app)) return real;\n" \
   "         return [...numbers.values()].filter(p => p.app === app).map(p => p.friend);\n" \
   "      };\n" \
   "      store.GetCountFriendsInGame = function(app) {\n" \
   "         const real = count.apply(this, arguments) || 0;\n" \
   "         return players.has(app) ? [...numbers.values()].filter(p => p.app === app).length : real;\n" \
   "      };\n" \
   "      links.BuildSteamURL = function(kind, what) {\n" \
   "         if (kind === 'SteamIDPage') {\n" \
   "            const p = numbers.get(Number(BigInt(what) & 0xffffffffn));\n" \
   "            if (p) return 'https://retroachievements.org/user/' + encodeURIComponent(p.name);\n" \
   "         }\n" \
   "         if (kind === 'CommunityFriendsThatPlay' && players.has(what))\n" \
   "            return 'https://retroachievements.org/game/' + players.get(what);\n" \
   "         return link.apply(this, arguments);\n" \
   "      };\n" \
   "      if (carded)\n" \
   "         Object.defineProperty(hover.prototype, 'RenderMiniProfile', { configurable: true, get() {\n" \
   "            const f = this.props && this.props.friend;\n" \
   "            const p = f && numbers.get(f.accountid);\n" \
   "            if (p) return () => card(p);\n" \
   "            return own.get ? own.get.call(this) : own.value.bind(this);\n" \
   "         } });\n" \
   "      undoFriends = () => {\n" \
   "         if (carded) Object.defineProperty(hover.prototype, 'RenderMiniProfile', own);\n" \
   "         store.GetFriendState = state;\n" \
   "         store.GetFriendsInGame = inGame;\n" \
   "         store.GetCountFriendsInGame = count;\n" \
   "         delete links.BuildSteamURL;\n" \
   "      };\n" \
   "      return true;\n" \
   "   };\n" \
   "   const ago = time => {\n" \
   "      const s = Math.max(0, Date.now() / 1000 - time);\n" \
   "      const [n, unit] = s < 3600 ? [s / 60, 'minute'] : s < 86400 ? [s / 3600, 'hour']\n" \
   "         : s < 30 * 86400 ? [s / 86400, 'day'] : s < 365 * 86400 ? [s / 2592000, 'month'] : [s / 31536000, 'year'];\n" \
   "      const whole = Math.max(1, Math.floor(n));\n" \
   "      return whole + ' ' + unit + (whole === 1 ? '' : 's') + ' ago';\n" \
   "   };\n" \
   "   /* Under a friend's name on the card over them. */\n" \
   "   const doing = p => p.on ? 'Online' : p.time ? 'Last online ' + ago(p.time) : 'On RetroAchievements';\n" \
   "   /* Steam's own card. For one who isn't playing, what they played last goes\n" \
   "      where it shows how long a friend played (a strip at its bottom); for one\n" \
   "      who is, the game's icon goes where its game area has a game's picture. */\n" \
   "   const card = p => {\n" \
   "      const e = react.createElement, strip = !p.on && p.last && strips && strips.PlaytimeSection;\n" \
   "      const icon = el => {\n" \
   "         const area = el && p.on && p.icon && css && css.gameLogo\n" \
   "            && el.querySelector('.' + CSS.escape(css.miniProfileGameContainer));\n" \
   "         if (!area || area.querySelector('img')) return;\n" \
   "         const img = el.ownerDocument.createElement('img');\n" \
   "         img.className = css.gameLogo;\n" \
   "         /* Square, in a wide place. */\n" \
   "         img.style.objectFit = 'contain';\n" \
   "         img.src = p.icon;\n" \
   "         area.prepend(img);\n" \
   "      };\n" \
   "      return e('div', { style: { display: 'contents' }, ref: icon },\n" \
   "         e(steamCard, { persona: p.friend.persona, data_loader: p.friend.miniProfileData, is_friend: true,\n" \
   "            bottom_section_additional: strip ? e('div', { className: strips.PlaytimeSection },\n" \
   "               p.icon && e('img', { className: strips.GameLogo, src: p.icon, style: { width: '55px' } }),\n" \
   "               e('div', { className: strips.PlaytimeContent }, e('div', null, 'Last played'),\n" \
   "                  e('div', null, e('span', { className: strips.TimePlayed }, p.last)))) : undefined }));\n" \
   "   };\n" \
   "   const own = hover && Object.getOwnPropertyDescriptor(hover.prototype, 'RenderMiniProfile');\n" \
   "   const carded = own && own.configurable && react && steamCard;\n" \
   "   /* What's drawn from who is in a game (the library's list) looks again: it\n" \
   "      watches Steam's map of them for that game. */\n" \
   "   const poke = app => {\n" \
   "      try {\n" \
   "         const seen = store.m_FriendsUIFriendStore.FriendGroupStore.m_mapGameGroups.hasMap_.get(app);\n" \
   "         if (seen) seen.reportChanged();\n" \
   "      } catch (e) { }\n" \
   "   };\n" \
   "   /* Made anew when they start or stop: what's drawn is drawn again then. */\n" \
   "   const make = p => {\n" \
   "      const own = new Friend(p.n), is = get => ({ get, configurable: true });\n" \
   "      const on = p.on, app = p.app;\n" \
   "      const avatar = is(() => p.avatar);\n" \
   "      const persona = Object.create(own.persona, {\n" \
   "         m_strPlayerName: is(() => p.name), m_bInitialized: is(() => true), m_bNameInitialized: is(() => true),\n" \
   "         avatar_url: avatar, avatar_url_medium: avatar, avatar_url_full: avatar,\n" \
   "         is_ingame: is(() => !!on), is_online: is(() => !!on),\n" \
   "         online_state: is(() => on ? 'ingame' : 'offline'), m_unGamePlayedAppID: is(() => app),\n" \
   "         GetCurrentGameName: is(() => () => {\n" \
   "            const o = app && appStore.GetAppOverviewByAppID(app);\n" \
   "            return o ? o.display_name : p.last;\n" \
   "         }),\n" \
   "         GetCurrentGameRichPresence: is(() => () => p.seen),\n" \
   "         HasCurrentGameRichPresence: is(() => () => !!p.seen),\n" \
   "         GetLocalizedOnlineStatus: is(() => () => doing(p))\n" \
   "      });\n" \
   "      p.friend = Object.create(own, {\n" \
   "         persona: is(() => persona), display_name: is(() => p.name), nickname: is(() => undefined),\n" \
   "         is_ready: is(() => true), is_blocked: is(() => false), is_friend: is(() => true),\n" \
   "         miniProfileData: is(() => ({ EnsureCommunityDataLoaded() { }, community_data: undefined }))\n" \
   "      });\n" \
   "   };\n" \
   "   const person = name => {\n" \
   "      if (people.has(name)) return people.get(name);\n" \
   "      const p = { name, n: FIRST + people.size, app: 0, avatar: '', seen: '', last: '', icon: '', time: 0, on: 0 };\n" \
   "      make(p);\n" \
   "      people.set(name, p);\n" \
   "      numbers.set(p.n, p);\n" \
   "      return p;\n" \
   "   };\n" \
   "   const drop = app => {\n" \
   "      games.delete(app);\n" \
   "      players.delete(app);\n" \
   "      mark(app);\n" \
   "      cache.removeQueries({ queryKey: playing(app) });\n" \
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
   "      version: 13,\n" \
   "      friends(app, game, list, tries = 0) {\n" \
   "         clearTimeout(later.get(app));\n" \
   "         if (!ready()) {\n" \
   "            /* For two minutes; then with the next ones. */\n" \
   "            if (store && links && plays && tries < 60)\n" \
   "               later.set(app, setTimeout(() => window.__fusebox.friends(app, game, list, tries + 1), 2000));\n" \
   "            return 'later';\n" \
   "         }\n" \
   "         if (!list.length) {\n" \
   "            if (players.delete(app)) {\n" \
   "               cache.removeQueries({ queryKey: playing(app) });\n" \
   "               mark(app);\n" \
   "               poke(app);\n" \
   "            }\n" \
   "            return 'ok';\n" \
   "         }\n" \
   "         const ever = list.map(([name, avatar, now, seen, last, time, on, icon]) => {\n" \
   "            const p = person(name);\n" \
   "            const was = p.on + ' ' + p.app + ' ' + p.last;\n" \
   "            Object.assign(p, { avatar, seen, last, time, icon, on: on ? 1 : 0 });\n" \
   "            if (now) p.app = app;\n" \
   "            else if (p.app === app) p.app = 0;\n" \
   "            if (was !== p.on + ' ' + p.app + ' ' + p.last) make(p);\n" \
   "            return p;\n" \
   "         }).filter(p => p.app !== app);\n" \
   "         const first = !players.has(app);\n" \
   "         players.set(app, game);\n" \
   "         cache.setQueryDefaults(playing(app), { gcTime: Infinity });\n" \
   "         cache.setQueryData(playing(app), { in_game: [], played_recently: [], in_wishlist: [], owns: [],\n" \
   "            played_ever: ever.map(p => ({ steamid: id64(p.n), minutes_played: 0, minutes_played_forever: 0 })),\n" \
   "            given: Date.now() }, { updatedAt: Date.now() + 10 * 365 * 864e5 });\n" \
   "         if (first) mark(app);\n" \
   "         poke(app);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      set(app, list) {\n" \
   "         const first = !games.has(app);\n" \
   "         games.set(app, list);\n" \
   "         put(app, list);\n" \
   "         if (first) mark(app);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      whole() {\n" \
   "         return [...games.keys()].every(app => names(app).every(queryKey =>\n" \
   "            ours(cache.getQueryCache().find({ queryKey }))));\n" \
   "      },\n" \
   "      keep(apps) {\n" \
   "         [...games.keys()].filter(a => !apps.includes(a)).forEach(drop);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      undo() {\n" \
   "         stop();\n" \
   "         later.forEach(clearTimeout);\n" \
   "         undoFriends();\n" \
   "         [...games.keys()].forEach(drop);\n" \
   "         page.prototype.GetSections = original;\n" \
   "         delete window.__fusebox;\n" \
   "         return 'ok';\n" \
   "      }\n" \
   "   };\n" \
   "   return 'ready';\n" \
   "})()"

#endif
