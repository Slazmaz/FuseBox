#ifndef FUSEBOX_STEAMUI_JS_H
#define FUSEBOX_STEAMUI_JS_H

/* Counted up with each change to the script: an older one is replaced. */
#define STEAMUI_VERSION "41"

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
 * ready yet when the account is: they're given once it is.
 *
 * Comparing achievements with a friend takes their name, picture and unlocks
 * from queries too. A picture's address is made from its hash: friends' hashes
 * are marked, and React's jsx gives their images the real address.
 *
 * The button that opens a game's emulator or launcher goes first among those
 * right of Play. The bar they're in can't be changed itself (MobX fixes each
 * one's render), so React's own jsxs, which makes it, adds the button. Clicks
 * wait here for the background FuseBox (asked), which knows the programs: the
 * page has only their names. Nobody asking, no button.
 *
 * Activity is Steam's own section, which shortcuts have a notice instead of.
 * Its store keeps each game's, so one is made here from events like those
 * Steam's servers send, and the store is kept from asking them about it. An
 * event is drawn only for a game the Steam store knows, by another query.
 * Inviting to a game: Steam has an item for it in friends' menus when the
 * user's own status says how to connect. Here it does while a game of FuseBox's
 * runs, for this page only, and what's clicked waits here like the button's.
 * A game that isn't in Steam is run by it all the same, for the overlay and the
 * rest: by a shortcut that's there only meanwhile.
 *
 * An accepted invite is a steam:// address this page opens: taken the same way.
 * Only one to Spacewar that FuseBox's message came with: others are Steam's, and
 * so is inviting while friends see a game of Steam's.
 * Steam knows the invite as one to Spacewar. Which game it's to is in the chat
 * message that comes with it (not shown here), and the invite is shown as one to that: by its
 * app id, for which the friends' own store of apps' names and pictures answers
 * here. One of FuseBox's that isn't in Steam gets a number no app has, and its
 * pictures from FuseBox, asked for like a click; one that isn't here either its
 * name, a word on that, and no pictures. One that came with the message is
 * over when the friend plays nothing any more, not when it isn't Spacewar.
 *
 * The overlay has a button for achievements, not for shortcuts: its entry among
 * the overlay's is changed. What's behind it (and Big Picture's item) asks the
 * game's details how many there are: they're answered for here. Opened while a
 * game runs, what was unlocked since is asked for like a click.
 *
 * Friends' achievements go in the achievements' section, under its link, and
 * leaderboards after it, a section made of Steam's parts: like the button.
 * Modules are
 * found by what's in them: their numbers change with every Steam update. */
#define STEAMUI_JS \
   "(() => {\n" \
   "   const me = window.App && App.m_CurrentUser && App.m_CurrentUser.strSteamID;\n" \
   "   if (!me || BigInt(me) % 4294967296n === 0n || !window.webpackChunksteamui || !window.appStore\n" \
   "         || !window.SteamUIStore || !SteamUIStore.WindowStore\n" \
   "         || !SteamUIStore.WindowStore.SteamUIWindows.length) return 'wait';\n" \
   "   if (window.__fusebox) {\n" \
   "      if (window.__fusebox.me === me && window.__fusebox.version === " STEAMUI_VERSION ") return 'ready';\n" \
   "      window.__fusebox.undo();\n" \
   "   }\n" \
   "   let req, page, cache, store, links, plays = false, keys = false;\n" \
   "   let react, hover, steamCard, css, strips, bar, jsx, column, Section, Body, entry;\n" \
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
   "         if (!bar && src.includes('AppButtonsContainer:') && src.includes('MenuButton:')) {\n" \
   "            const x = req(id);\n" \
   "            bar = x.AppButtonsContainer ? x : pick(id, y => y.AppButtonsContainer);\n" \
   "         }\n" \
   "         if (!column && src.includes('RightColumn:') && src.includes('Spotlights:')) {\n" \
   "            const x = req(id);\n" \
   "            column = x.RightColumn ? x : pick(id, y => y.RightColumn);\n" \
   "         }\n" \
   "         if (!Section && src.includes('AppDetailsSectionHasLabel') && src.includes('rightColumnSection:')) {\n" \
   "            Section = pick(id, x => typeof x === 'object' && x.$$typeof);\n" \
   "            Body = pick(id, x => typeof x === 'function' && String(x).includes('.BodyGutter'));\n" \
   "         }\n" \
   "         /* The overlay's button: the same entry each time it's asked for. */\n" \
   "         if (!entry && src.includes('#AppOverlay_Toolbar_Achievements')) {\n" \
   "            const of = pick(id, x => typeof x === 'function' && x('Achievements').buttonVisible), x = of && of('Achievements');\n" \
   "            if (x && of('Achievements') === x) entry = x;\n" \
   "         }\n" \
   "         if (!jsx && /\\.jsxs=/.test(src)) {\n" \
   "            const x = req(id);\n" \
   "            if (x.jsx && x.jsxs) jsx = x;\n" \
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
   "      const sections = original.apply(this, arguments), app = overview && overview.appid;\n" \
   "      if (!pages.includes(this)) pages = [this, ...pages].slice(0, 4);\n" \
   "      if (!games.has(app) && !acts.has(app)) return sections;\n" \
   "      const more = new Set(sections);\n" \
   "      if (games.has(app)) more.add('achievements');\n" \
   "      if (players.has(app)) more.add('friends');\n" \
   "      if (acts.has(app)) {\n" \
   "         more.delete('nonsteam');\n" \
   "         more.add('activity');\n" \
   "      }\n" \
   "      return more;\n" \
   "   };\n" \
   "   /* The overlay. Details are made anew when Steam changes them, so what's\n" \
   "      answered instead is kept by them, and by the achievements it's of. */\n" \
   "   const counts = new WeakMap(), facts = window.appDetailsStore, buttonWas = entry && entry.buttonVisible;\n" \
   "   const counted = (app, details) => {\n" \
   "      const list = games.get(app), kept = details && counts.get(details);\n" \
   "      if (!details || !list) return details;\n" \
   "      if (kept && kept.list === list) return kept.told;\n" \
   "      const achievements = { vecHighlight: [], vecUnachieved: [], vecAchievedHidden: [], nTotal: list.length,\n" \
   "         nAchieved: list.filter(a => a[5]).length };\n" \
   "      const told = new Proxy(details, { get: (o, key) => key === 'achievements' ? achievements : Reflect.get(o, key) });\n" \
   "      counts.set(details, { list, told });\n" \
   "      return told;\n" \
   "   };\n" \
   "   /* What's drawn from them looks again: an overlay may be there already. */\n" \
   "   const recount = app => {\n" \
   "      try {\n" \
   "         const data = facts.m_mapAppData.get(app);\n" \
   "         if (data && data.details) data.details = Object.assign({}, data.details);\n" \
   "      } catch (e) { }\n" \
   "   };\n" \
   "   const overlay = !!(entry && facts && facts.GetAppDetails && facts.GetAchievements);\n" \
   "   if (overlay) {\n" \
   "      const { GetAppDetails, GetAchievements } = facts;\n" \
   "      facts.GetAppDetails = function(app) { return counted(app, GetAppDetails.apply(this, arguments)); };\n" \
   "      facts.GetAchievements = function(app) {\n" \
   "         const details = games.has(app) && this.GetAppDetails(app);\n" \
   "         return details ? details.achievements : GetAchievements.apply(this, arguments);\n" \
   "      };\n" \
   "      entry.buttonVisible = function(o) { return !!(o && games.has(o.appid)) || buttonWas.apply(this, arguments); };\n" \
   "   }\n" \
   "   const names = app => [['achievements', app, 'schema'], ['achievements', app, 'globalpercentages'],\n" \
   "      ['achievements', app, 'user_achievements', me]];\n" \
   "   /* Friends: by name, and by the number they go by here; each game's. */\n" \
   "   const people = new Map(), numbers = new Map(), players = new Map(), later = new Map();\n" \
   "   let Friend, state, inGame, undoFriends = () => {};\n" \
   "   const FIRST = 0xffff0000;\n" \
   "   /* Each game's activity as it was given, and those whose Steam has. */\n" \
   "   const given = new Map(), acts = new Set(), building = new Map(), feed = window.appActivityStore;\n" \
   "   const ACHIEVED = 2, ADDED = 3, PLAYED = 30;\n" \
   "   const id64 = n => (76561197960265728n + BigInt(n)).toString();\n" \
   "   const playing = app => ['FriendsGameplayInfo', me, app, true];\n" \
   "   const ready = () => {\n" \
   "      if (Friend || !store || !links || !plays) return !!Friend;\n" \
   "      try { Friend = store.GetFriendState(store.currentUserSteamID).constructor; } catch (e) { return false; }\n" \
   "      state = store.GetFriendState;\n" \
   "      inGame = store.GetFriendsInGame;\n" \
   "      const count = store.GetCountFriendsInGame;\n" \
   "      const link = links.BuildSteamURL, resolve = links.ResolveURL;\n" \
   "      links.ResolveURL = function(kind, who, app) {\n" \
   "         return (kind === 'UserAchievementsPage' && compared(who, app)) || resolve.apply(this, arguments);\n" \
   "      };\n" \
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
   "      links.BuildSteamURL = function(kind, what, ...more) {\n" \
   "         /* Someone's achievements, or the user's own. */\n" \
   "         const there = kind === 'UserAchievementsPage' ? compared(what, more[0])\n" \
   "            : kind === 'SteamIDAchievementsPage' ? compared(0, what) : '';\n" \
   "         if (there) return there;\n" \
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
   "         delete links.ResolveURL;\n" \
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
   "   /* The address of a game's achievements there, next to a friend's. */\n" \
   "   const compared = (who, app) => {\n" \
   "      const g = given.get(Number(app)), p = who && numbers.get(Number(BigInt(who) & 0xffffffffn));\n" \
   "      if (!g || !g.id) return '';\n" \
   "      return 'https://retroachievements.org/' + (p ? 'user/' + encodeURIComponent(p.name) + '/game/' + g.id + '/compare'\n" \
   "         : 'game/' + g.id);\n" \
   "   };\n" \
   "   /* Them as a query has people, by a hash that says who. */\n" \
   "   const MARK = 'fb'.repeat(16);\n" \
   "   const details = p => {\n" \
   "      const key = ['PlayerLinkDetails', id64(p.n)], hash = new Uint8Array(20).fill(0xfb);\n" \
   "      [24, 16, 8, 0].forEach((bits, i) => { hash[16 + i] = p.n >>> bits; });\n" \
   "      cache.setQueryDefaults(key, { gcTime: Infinity });\n" \
   "      cache.setQueryData(key, { public_data: { steamid: key[1], persona_name: p.name, visibility_state: 3,\n" \
   "         profile_state: 1, sha_digest_avatar: hash }, private_data: { persona_state: p.on ? 1 : 0 } },\n" \
   "         { updatedAt: Date.now() + 10 * 365 * 864e5 });\n" \
   "   };\n" \
   "   /* What a friend unlocked of a game's, as the user's is. */\n" \
   "   const theirs = (app, p) => {\n" \
   "      const g = given.get(app), list = games.get(app);\n" \
   "      if (!g || !list) return;\n" \
   "      const ids = new Set(list.map(a => a[0])), got = g.wins.filter(w => w[1] === p.name && ids.has(w[0]));\n" \
   "      cache.setQueryData([...names(app)[2].slice(0, 3), id64(p.n)], { appid: app, steamid: id64(p.n), schema_hash: 1,\n" \
   "         achievements: Object.fromEntries(got.map(w => [w[0], { internal_key: w[0], unlocked: true, unlock_time: w[2] }])),\n" \
   "         groups: { 0: { groupid: 0, is_achievable: true, completed_achievements: got.length,\n" \
   "            is_completed: got.length === list.length } } }, { updatedAt: Date.now() + 10 * 365 * 864e5 });\n" \
   "   };\n" \
   "   const person = name => {\n" \
   "      if (people.has(name)) return people.get(name);\n" \
   "      const p = { name, n: FIRST + people.size, app: 0, avatar: '', seen: '', last: '', icon: '', time: 0, on: 0 };\n" \
   "      make(p);\n" \
   "      people.set(name, p);\n" \
   "      numbers.set(p.n, p);\n" \
   "      details(p);\n" \
   "      return p;\n" \
   "   };\n" \
   "   const drop = app => {\n" \
   "      games.delete(app);\n" \
   "      if (overlay) recount(app);\n" \
   "      players.delete(app);\n" \
   "      if (boards.delete(app)) redraw();\n" \
   "      mark(app);\n" \
   "      cache.removeQueries({ queryKey: playing(app) });\n" \
   "      names(app).forEach(queryKey => cache.removeQueries({ queryKey }));\n" \
   "      cache.removeQueries({ queryKey: names(app)[2].slice(0, 3) });\n" \
   "   };\n" \
   "   /* Unlocked while it was looked: about then, until it's known better. */\n" \
   "   const lately = new Map();\n" \
   "   const put = (app, list) => {\n" \
   "      const at = { updatedAt: Date.now() + 10 * 365 * 864e5 };\n" \
   "      const badge = 'https://media.retroachievements.org/Badge/';\n" \
   "      const key = names(app), got = list.filter(a => a[5]);\n" \
   "      /* When each was, where that's known: Steam sorts by it. */\n" \
   "      const when = new Map((given.has(app) ? given.get(app).wins : []).filter(w => !w[1]).map(w => [w[0], w[2]]));\n" \
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
   "            unlock_time: when.get(a[0]) || lately.get(a[0]) || 0 }])),\n" \
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
   "   /* Activity. */\n" \
   "   const quiet = ['FetchLatestActivity', 'FetchLatestActivityFromServer', 'FetchActivityHistory'];\n" \
   "   if (feed) quiet.forEach(name => {\n" \
   "      const was = feed[name];\n" \
   "      feed[name] = function(app) {\n" \
   "         return given.has(app) ? Promise.resolve() : was.apply(this, arguments);\n" \
   "      };\n" \
   "   });\n" \
   "   /* Steam tells days by today's offset from UTC, whatever it was then: times\n" \
   "      are moved by the difference, and back once they're in their days. */\n" \
   "   const moved = time => (new Date().getTimezoneOffset() - new Date(time * 1000).getTimezoneOffset()) * 60;\n" \
   "   const event = (app, type, time, who, names) => ({ eventtype: () => type, eventtime: () => time + moved(time),\n" \
   "      steamid_actor: () => who, gameid: () => String(app), appids: () => [], achievement_names: () => names,\n" \
   "      toObject: () => ({}) });\n" \
   "   const build = async (app, tries = 0) => {\n" \
   "      const g = given.get(app);\n" \
   "      clearTimeout(building.get(app));\n" \
   "      if (!g || !feed || !feed.m_mapAppActivity) return;\n" \
   "      const some = feed.m_mapAppActivity.values().next().value;\n" \
   "      if (!some) {\n" \
   "         /* Steam makes an empty one for a game that's asked for. */\n" \
   "         if (!tries) feed.RequestRestoreActivity(app);\n" \
   "         if (tries < 30) building.set(app, setTimeout(() => build(app, tries + 1), 1000));\n" \
   "         return;\n" \
   "      }\n" \
   "      const at = { updatedAt: Date.now() + 10 * 365 * 864e5 }, o = appStore.GetAppOverviewByAppID(app);\n" \
   "      const item = ['StoreItem', 'app_' + app], spot = ['AppSpotlightEvent', app];\n" \
   "      [item, spot].forEach(key => cache.setQueryDefaults(key, { gcTime: Infinity }));\n" \
   "      cache.setQueryData([...item, 'default_info'], { appid: app, id: app, item_type: 0, success: 1, visible: true,\n" \
   "         type: 0, name: o ? o.display_name : '' }, at);\n" \
   "      cache.setQueryData([...item, 'include_assets'], null, at);\n" \
   "      [true, false].forEach(seen => cache.setQueryData([...spot, seen], null, at));\n" \
   "      const a = new some.constructor(app), known = new Map((games.get(app) || []).map(x => ['RA_' + x[0],\n" \
   "         { strID: 'RA_' + x[0], strName: x[1], strDescription: x[2], flAchieved: x[4], bHidden: false,\n" \
   "            strImage: 'https://media.retroachievements.org/Badge/' + x[3] + '.png' }]));\n" \
   "      a.m_AchievementMap.set(app, known);\n" \
   "      g.missing = false;\n" \
   "      const who = name => {\n" \
   "         if (!name) return me;\n" \
   "         if (Friend) return id64(person(name).n);\n" \
   "         g.missing = true;\n" \
   "         return '';\n" \
   "      };\n" \
   "      const events = g.wins.map(([id, name, time]) => event(app, ACHIEVED, time, who(name), ['RA_' + id]))\n" \
   "         .filter(e => e.steamid_actor() && known.has(e.achievement_names()[0]));\n" \
   "      if (g.added) events.push(event(app, ADDED, g.added, me, []));\n" \
   "      /* Not the first time if something was unlocked before, or who knows when. */\n" \
   "      const when = new Map(g.wins.filter(w => !w[1]).map(w => [w[0], w[2]]));\n" \
   "      if (g.played && (games.get(app) || []).every(x => !x[5] || when.get(x[0]) >= g.played))\n" \
   "         events.push(event(app, PLAYED, g.played, me, []));\n" \
   "      events.sort((x, y) => y.eventtime() - x.eventtime());\n" \
   "      await Promise.all(events.map(e => a.AddUserNewsEvent(e)));\n" \
   "      if (given.get(app) !== g) return;\n" \
   "      a.SortEvents();\n" \
   "      a.m_bNoMoreHistoryAvailable = true;\n" \
   "      /* Comments are Steam's servers'. */\n" \
   "      a.m_mapActivityByDay.forEach(d => d.allEvents.forEach(e => {\n" \
   "         e.BSupportsCommentThreads = () => false;\n" \
   "         e.rtEventTime -= moved(e.rtEventTime);\n" \
   "         if (e.achievements) e.achievements.forEach(x => { x.rtUnlocked -= moved(x.rtUnlocked); });\n" \
   "      }));\n" \
   "      feed.m_mapAppActivity.set(app, a);\n" \
   "      if (!acts.has(app)) {\n" \
   "         acts.add(app);\n" \
   "         mark(app);\n" \
   "      }\n" \
   "   };\n" \
   "   const forget = app => {\n" \
   "      given.delete(app);\n" \
   "      clearTimeout(building.get(app));\n" \
   "      if (!acts.delete(app)) return;\n" \
   "      feed.m_mapAppActivity.delete(app);\n" \
   "      cache.removeQueries({ queryKey: ['StoreItem', 'app_' + app] });\n" \
   "      cache.removeQueries({ queryKey: ['AppSpotlightEvent', app] });\n" \
   "      mark(app);\n" \
   "   };\n" \
   "   /* The buttons: each game's program's name, those drawn, and clicks. */\n" \
   "   const opens = new Map(), buttons = new Set(), clicks = [], boards = new Map();\n" \
   "   let heard = false, deaf, wake, jsxs, mine, plain, pictured, clicked = 0;\n" \
   "   const redraw = () => buttons.forEach(b => { try { b.forceUpdate(); } catch (e) { } });\n" \
   "   const said = new Map();\n" \
   "   const send = text => {\n" \
   "      if (Date.now() - (said.get(text) || 0) < 1500) return;\n" \
   "      said.set(text, Date.now());\n" \
   "      if (wake) wake(text);\n" \
   "      else clicks.push(text);\n" \
   "   };\n" \
   "   const open = app => {\n" \
   "      if (Date.now() - clicked < 1500) return;\n" \
   "      clicked = Date.now();\n" \
   "      send('open ' + app);\n" \
   "   };\n" \
   "   if (react && bar && jsx && bar.MenuButton) {\n" \
   "      const Open = class extends react.Component {\n" \
   "         componentDidMount() { buttons.add(this); }\n" \
   "         componentWillUnmount() { buttons.delete(this); }\n" \
   "         render() {\n" \
   "            const app = this.props.app, name = heard && opens.get(app);\n" \
   "            if (!name) return null;\n" \
   "            return react.createElement('div', { className: bar.MenuButton, role: 'button', tabIndex: 0,\n" \
   "               onClick: () => open(app), onKeyDown: e => { if (e.key === 'Enter') open(app); },\n" \
   "               style: { width: 'auto', padding: '0 12px', marginRight: '10px', fontSize: '14px',\n" \
   "                  whiteSpace: 'nowrap', display: 'flex', alignItems: 'center' } }, 'Open ' + name);\n" \
   "         }\n" \
   "      };\n" \
   "      /* Leaderboards: the user's place on each, and friends'. */\n" \
   "      const e = react.createElement, dim = '#8b929a', cut = { overflow: 'hidden', textOverflow: 'ellipsis',\n" \
   "         whiteSpace: 'nowrap', flex: '1 1 0', minWidth: 0 };\n" \
   "      const line = (key, left, right, style) => e('div', { key, style: { display: 'flex', gap: '10px',\n" \
   "         alignItems: 'center', ...style } }, left, e('div', { style: { flexShrink: 0 } }, right));\n" \
   "      const row = ([id, title, about, total, places]) => {\n" \
   "         const own = places.find(p => !p[0]);\n" \
   "         const rank = p => '#' + p[2].toLocaleString() + ' of ' + total.toLocaleString();\n" \
   "         return e('div', { key: id, title: about, role: 'link', tabIndex: 0, style: { padding: '7px 0', cursor: 'pointer' },\n" \
   "            onClick: () => SteamClient.URL.ExecuteSteamURL(\n" \
   "               'steam://openurl/https://retroachievements.org/leaderboardinfo.php?i=' + id) },\n" \
   "            line('title', e('div', { style: cut }, title), own ? own[1] : '', { color: '#dcdedf', fontSize: '14px' }),\n" \
   "            line('rank', e('div', { style: cut }, about), own ? rank(own) : '', { color: dim, fontSize: '12px', marginTop: '2px' }),\n" \
   "            places.filter(p => p[0]).map(p => {\n" \
   "               const who = people.get(p[0]);\n" \
   "               return line(p[0], e('div', { style: { ...cut, display: 'flex', gap: '6px', alignItems: 'center' } },\n" \
   "                  who && who.avatar && e('img', { src: who.avatar, style: { width: '16px', height: '16px' } }), p[0]),\n" \
   "                  p[1] + '  ' + rank(p), { color: dim, fontSize: '12px', marginTop: '4px' });\n" \
   "            }));\n" \
   "      };\n" \
   "      /* Friends' achievements: how many of the game's each has. */\n" \
   "      const progress = app => {\n" \
   "         const ids = new Set((games.get(app) || []).map(a => a[0])), got = new Map();\n" \
   "         (given.has(app) ? given.get(app).wins : []).forEach(([id, name]) => {\n" \
   "            if (name && ids.has(id)) got.set(name, (got.get(name) || new Set()).add(id));\n" \
   "         });\n" \
   "         return [...got].map(([name, has]) => ({ name, has: has.size, of: ids.size })).sort((x, y) => y.has - x.has);\n" \
   "      };\n" \
   "      const friendRow = app => ({ name, has, of }) => {\n" \
   "         const who = people.get(name), part = Math.round(has * 1000 / of) / 10;\n" \
   "         return e('div', { key: name, role: 'link', tabIndex: 0, style: { display: 'flex', gap: '10px',\n" \
   "            padding: '6px 0', cursor: 'pointer', alignItems: 'center' }, onClick: () => SteamClient.URL.ExecuteSteamURL(\n" \
   "               'steam://openurl/' + compared(who ? id64(who.n) : 0, app)) },\n" \
   "            who && who.avatar && e('img', { src: who.avatar, style: { width: '32px', height: '32px', flexShrink: 0 } }),\n" \
   "            e('div', { style: { flex: '1 1 0', minWidth: 0 } },\n" \
   "               line('name', e('div', { style: cut }, name), [has + '/' + of + ' ', e('span', { key: 'part',\n" \
   "                  style: { color: dim } }, '(' + part + '%)')], { color: '#dcdedf', fontSize: '14px' }),\n" \
   "               e('div', { style: { height: '8px', borderRadius: '4px', background: 'rgba(0, 0, 0, 0.5)',\n" \
   "                  marginTop: '5px', overflow: 'hidden' } }, e('div', { style: { height: '100%', borderRadius: '4px',\n" \
   "                  width: part + '%', background: '#1a9fff' } }))));\n" \
   "      };\n" \
   "      const Progress = class extends react.Component {\n" \
   "         constructor(props) { super(props); this.state = { all: false }; }\n" \
   "         componentDidMount() { buttons.add(this); }\n" \
   "         componentWillUnmount() { buttons.delete(this); }\n" \
   "         render() {\n" \
   "            const list = progress(this.props.app), all = this.state.all;\n" \
   "            if (!list.length) return null;\n" \
   "            return e('div', { style: { padding: '10px', borderTop: '1px solid rgba(255, 255, 255, 0.08)' } },\n" \
   "               e('div', { style: { color: dim, fontSize: '13px', marginBottom: '4px' } }, 'Friends\\' achievements'),\n" \
   "               (all ? list : list.slice(0, 5)).map(friendRow(this.props.app)),\n" \
   "               list.length > 5 && e('div', { role: 'button', tabIndex: 0, style: { color: dim, fontSize: '13px',\n" \
   "                  textAlign: 'right', cursor: 'pointer', paddingTop: '6px' },\n" \
   "                  onClick: () => this.setState({ all: !all }) }, all ? 'Show less' : 'View all ' + list.length));\n" \
   "         }\n" \
   "      };\n" \
   "      const Boards = class extends react.Component {\n" \
   "         constructor(props) { super(props); this.state = { all: false }; }\n" \
   "         componentDidMount() { buttons.add(this); }\n" \
   "         componentWillUnmount() { buttons.delete(this); }\n" \
   "         render() {\n" \
   "            const list = boards.get(this.props.app), all = this.state.all;\n" \
   "            if (!list || !list.length || !Section || !Body) return null;\n" \
   "            return e(Section, { label: 'Leaderboards', rightColumnSection: true, availableOffline: true,\n" \
   "               primaryAction: list.length > 5 && { label: all ? 'Show Less' : 'View All ' + list.length,\n" \
   "                  action: () => this.setState({ all: !all }) } },\n" \
   "               e(Body, null, (all ? list : list.slice(0, 5)).map(row)));\n" \
   "         }\n" \
   "      };\n" \
   "      plain = jsx.jsx;\n" \
   "      pictured = jsx.jsx = function(type, props, ...more) {\n" \
   "         if (type === 'img' && props && typeof props.src === 'string' && props.src.includes(MARK)) {\n" \
   "            const p = numbers.get(parseInt(props.src.substr(props.src.indexOf(MARK) + MARK.length, 8), 16));\n" \
   "            if (p && p.avatar) props = { ...props, src: p.avatar };\n" \
   "         }\n" \
   "         else [type, props] = invited(type, props);\n" \
   "         /* How many friends play, by the friends to compare to: a link to a page\n" \
   "            of Steam's that isn't there, so only text. */\n" \
   "         if (props && typeof props.href === 'string' && props.href.includes('/friendsthatplay/')\n" \
   "               && players.has(Number(props.href.split('/friendsthatplay/')[1])))\n" \
   "            props = { ...props, href: undefined, underline: 'none', style: { ...props.style, pointerEvents: 'none' } };\n" \
   "         return plain.call(this, type, props, ...more);\n" \
   "      };\n" \
   "      jsxs = jsx.jsxs;\n" \
   "      mine = jsx.jsxs = function(type, props, ...more) {\n" \
   "         if (column && type === 'div' && props && props.className === column.RightColumn && Array.isArray(props.children)) {\n" \
   "            const part = c => c && c.props && c.props.children && c.props.children.props;\n" \
   "            const at = props.children.findIndex(c => part(c) && part(c).name === 'achievements');\n" \
   "            const on = at >= 0 && part(props.children[at]).parent;\n" \
   "            if (on && on.props && on.props.overview) {\n" \
   "               const app = on.props.overview.appid;\n" \
   "               props = { ...props, children: [...props.children.slice(0, at + 1), e(Boards, { key: 'fusebox', app }),\n" \
   "                  ...props.children.slice(at + 1)] };\n" \
   "            }\n" \
   "         }\n" \
   "         if (type === 'div' && props && props.className === bar.AppButtonsContainer && Array.isArray(props.children)) {\n" \
   "            const of = props.children.find(c => c && c.props && c.props.overview);\n" \
   "            if (of) props = { ...props, children: [\n" \
   "               react.createElement(Open, { key: 'fusebox', app: of.props.overview.appid }), ...props.children] };\n" \
   "         }\n" \
   "         /* The activity's box to post to Steam friends: not about these. */\n" \
   "         else if (props && props.children && props.children.length === 2) {\n" \
   "            const list = props.children[1];\n" \
   "            if (list && list.props && list.props.ShowMoreContent && acts.has(list.props.appid))\n" \
   "               props = { ...props, children: [false, list] };\n" \
   "            /* The achievements' section: its number on top, and the rest. */\n" \
   "            const top = props.children[0];\n" \
   "            if (Body && list && list.type === Body && top && top.props && top.props.summary && given.has(top.props.appid))\n" \
   "               props = { ...props, children: [top, list, e(Progress, { key: 'fusebox', app: top.props.appid })] };\n" \
   "         }\n" \
   "         [type, props] = stamped(type, props);\n" \
   "         return jsxs.call(this, type, props, ...more);\n" \
   "      };\n" \
   "   }\n" \
   "   /* Playing with friends. */\n" \
   "   const FUSEBOX = '+fusebox', SPACEWAR = 480, lanGames = new Map(), onces = new Map(), passed = new Set();\n" \
   "   const undoLan = [];\n" \
   "   let ui = window.g_FriendsUIApp, lanReady = false;\n" \
   "   /* What runs, as Steam's library and as the friends' list have it: a\n" \
   "      shortcut's app id is the upper half of its game id. */\n" \
   "   const running = () => {\n" \
   "      const ids = [];\n" \
   "      try { (SteamUIStore.RunningApps || []).forEach(x => ids.push(x.appid)); } catch (e) { }\n" \
   "      try {\n" \
   "         const id = BigInt(ui.FriendStore.self.persona.m_gameid || 0), app = Number(id >> 32n);\n" \
   "         if (app && !ids.includes(app)) ids.push(app);\n" \
   "      } catch (e) { }\n" \
   "      return ids;\n" \
   "   };\n" \
   "   /* Opened over a game: what was unlocked by now. */\n" \
   "   const looks = new Map();\n" \
   "   const look = (pid, app, active) => {\n" \
   "      const of = games.has(app) ? app : running().find(a => games.has(a));\n" \
   "      if (!active || !of || !heard || Date.now() - (looks.get(of) || 0) < 15000) return;\n" \
   "      looks.set(of, Date.now());\n" \
   "      send('unlocks ' + of);\n" \
   "   };\n" \
   "   let opened;\n" \
   "   try { opened = overlay && SteamClient.Overlay.RegisterForOverlayActivated(look); } catch (e) { }\n" \
   "   const lanGame = () => running().find(app => lanGames.has(app)) || 0;\n" \
   "   /* Friends see a game of Steam's: inviting is to that one. */\n" \
   "   const steamGame = () => ![0, SPACEWAR].includes(ui.FriendStore.self.persona.m_unGamePlayedAppID || 0);\n" \
   "   const take = url => {\n" \
   "      const m = /^steam:\\/\\/joinlobby\\/480\\/(\\d+)\\/(\\d+)/.exec(String(url));\n" \
   "      if (!m || !heard || passed.has(m[1]) || !toLobby(m[1])) return false;\n" \
   "      send('join ' + m[1] + ' ' + m[2]);\n" \
   "      return true;\n" \
   "   };\n" \
   "   /* What was said of invites: by whom, when, to which game. */\n" \
   "   const SAID = /^Invited you to play (.+) with FuseBox$/, told = [];\n" \
   "   const bare = name => name.toLowerCase().replace(/[^a-z0-9]/g, '') || name.toLowerCase().trim();\n" \
   "   const gameNamed = name => {\n" \
   "      for (const [app, names] of lanGames) if (names.some(n => bare(n) === bare(name))) return app;\n" \
   "      for (const [id, names] of inLibrary) if (names.some(n => bare(n) === bare(name))) return OTHER + id;\n" \
   "      return 0;\n" \
   "   };\n" \
   "   const marked = invite => !!invite && Object.hasOwn(invite, 'fusebox');\n" \
   "   const toldOf = invite => {\n" \
   "      const off = t => Math.abs(t.time - invite.rtTimestamp);\n" \
   "      return told.filter(t => t.who === invite.unAccountIDInviter && off(t) <= 120)\n" \
   "         .sort((x, y) => off(x) - off(y))[0];\n" \
   "   };\n" \
   "   const fromFusebox = invite => marked(invite) && !!toldOf(invite);\n" \
   "   /* Games that aren't here, by numbers no app has; those of FuseBox's that\n" \
   "      aren't in Steam, by others, and their pictures. */\n" \
   "   const ABSENT = 2 ** 33, absent = [], OTHER = 2 ** 34, inLibrary = new Map(), art = new Map();\n" \
   "   const otherNames = app => app >= OTHER ? inLibrary.get(app - OTHER) : undefined;\n" \
   "   const artOf = app => {\n" \
   "      const id = app - OTHER;\n" \
   "      const has = art.get(id);\n" \
   "      /* Again if none came. */\n" \
   "      if (!has || (has.at && Date.now() - has.at > 60000)) {\n" \
   "         art.set(id, { at: Date.now() });\n" \
   "         send('art ' + id);\n" \
   "      }\n" \
   "      return art.get(id);\n" \
   "   };\n" \
   "   const absentName = app => app >= ABSENT && app < OTHER ? absent[app - ABSENT] : undefined;\n" \
   "   const absentNamed = name => {\n" \
   "      if (!absent.includes(name)) absent.push(name);\n" \
   "      return ABSENT + absent.indexOf(name);\n" \
   "   };\n" \
   "   /* Whether FuseBox's message came with an invite to it: others are Steam's. */\n" \
   "   const toLobby = lobby => {\n" \
   "      try {\n" \
   "         for (const invites of ui.ChatStore.GameInviteStore.m_mapGameInvites.values())\n" \
   "            for (const invite of invites.values())\n" \
   "               if (String(invite.lobbyID) === lobby && toldOf(invite)) return true;\n" \
   "      } catch (e) { }\n" \
   "      return false;\n" \
   "   };\n" \
   "   const gameOf = invite => {\n" \
   "      const t = toldOf(invite);\n" \
   "      return t ? gameNamed(t.name) || absentNamed(t.name) : 0;\n" \
   "   };\n" \
   "   /* One to Spacewar, as to the game that's said of it, if any is. */\n" \
   "   const markInvite = invite => {\n" \
   "      if (!invite || !invite.lobbyID || Number(invite.appID) !== SPACEWAR || marked(invite)) return;\n" \
   "      const url = invite.inviteString, was = Object.getOwnPropertyDescriptor(invite, 'appID');\n" \
   "      if (!was || !was.configurable) return;\n" \
   "      Object.defineProperty(invite, 'fusebox', { configurable: true, value: true });\n" \
   "      Object.defineProperty(invite, 'inviteString', { configurable: true, get: () => url });\n" \
   "      Object.defineProperty(invite, 'appID', { configurable: true, get: () => gameOf(invite) || SPACEWAR });\n" \
   "      undoLan.push(() => {\n" \
   "         Object.defineProperty(invite, 'appID', was);\n" \
   "         delete invite.inviteString;\n" \
   "         delete invite.fusebox;\n" \
   "      });\n" \
   "   };\n" \
   "   /* An invite that was drawn already is drawn again. */\n" \
   "   const again = who => {\n" \
   "      try {\n" \
   "         const latest = ui.ChatStore.GameInviteStore.m_mapLatestInvite, invite = latest.get(who);\n" \
   "         if (fromFusebox(invite)) {\n" \
   "            latest.delete(who);\n" \
   "            latest.set(who, invite);\n" \
   "         }\n" \
   "      } catch (e) { }\n" \
   "   };\n" \
   "   const note = (who, time, text) => {\n" \
   "      const m = SAID.exec(String(text).trim());\n" \
   "      if (!m || told.some(t => t.who === who && t.time === time)) return;\n" \
   "      told.push({ who, time, name: m[1] });\n" \
   "      if (told.length > 200) told.shift();\n" \
   "      again(who);\n" \
   "   };\n" \
   "   /* The message is for friends without FuseBox: not drawn or told of here. */\n" \
   "   const quieted = new WeakSet();\n" \
   "   const quietChat = chat => {\n" \
   "      if (!chat || quieted.has(chat)) return;\n" \
   "      quieted.add(chat);\n" \
   "      /* Those that came before this did. */\n" \
   "      try { chat.m_rgChatMessages.forEach(m => note(m.unAccountID, m.rtTimestamp, m.strMessage)); } catch (e) { }\n" \
   "      const list = chat.m_ChatMessageBlockList, said = m => SAID.test(String(m && m.strMessage).trim());\n" \
   "      try {\n" \
   "         around(chat, 'AddNewChatMsgAndNotify', function(steams, who, time, at, text, ...more) {\n" \
   "            if (!SAID.test(String(text).trim())) return steams(who, time, at, text, ...more);\n" \
   "            note(who, time, text);\n" \
   "            this.AppendChatMsg(who, time, at, text);\n" \
   "         });\n" \
   "         around(list, 'AppendNewMessage', function(steams, m) { return said(m) ? undefined : steams(m); });\n" \
   "         around(list, 'BuildMessageBlocks', function(steams, all, ...more) {\n" \
   "            return steams(all.filter(m => !said(m)), ...more);\n" \
   "         });\n" \
   "         const again = () => {\n" \
   "            if (chat.m_rgChatMessages.some(said)) list.BuildMessageBlocks(chat.m_rgChatMessages);\n" \
   "         };\n" \
   "         again();\n" \
   "         undoLan.unshift(again);\n" \
   "      } catch (e) { }\n" \
   "   };\n" \
   "   const quietChats = () => {\n" \
   "      try { ui.ChatStore.FriendChatStore.m_rgFriendChats.forEach(quietChat); } catch (e) { }\n" \
   "   };\n" \
   "   /* A game of FuseBox's as the friends' part knows apps. */\n" \
   "   const infos = new Map(), overview = app => appStore.GetAppOverviewByAppID(app);\n" \
   "   const infoOf = (app, like) => {\n" \
   "      if (infos.has(app)) return infos.get(app);\n" \
   "      const is = get => ({ get, configurable: true }), o = () => overview(app);\n" \
   "      const icon = () => {\n" \
   "         if (otherNames(app)) return artOf(app).icon || undefined;\n" \
   "         try { return o() && appStore.GetIconURLForApp(o()) || undefined; } catch (e) { return undefined; }\n" \
   "      };\n" \
   "      const missing = absentName(app);\n" \
   "      const info = Object.create(like, { appid: is(() => app), icon_url: is(icon), icon_url_no_default: is(icon),\n" \
   "         name: is(() => missing !== undefined ? missing + ' (you don\\'t have this game in your library)'\n" \
   "            : otherNames(app) ? otherNames(app)[0] : o() ? o().display_name\n" \
   "            : lanGames.has(app) ? lanGames.get(app)[0] : '') });\n" \
   "      infos.set(app, info);\n" \
   "      return info;\n" \
   "   };\n" \
   "   const wide = app => {\n" \
   "      if (otherNames(app)) return artOf(app).wide || '';\n" \
   "      try {\n" \
   "         const o = overview(app);\n" \
   "         return appStore.GetCustomLandcapeImageURLs(o)[0] || appStore.GetIconURLForApp(o) || '';\n" \
   "      } catch (e) { return ''; }\n" \
   "   };\n" \
   "   /* The friend, as in the game the invite is to. */\n" \
   "   const hosts = new Map();\n" \
   "   const hostOf = (friend, app) => {\n" \
   "      const key = friend.accountid + ' ' + app, is = get => ({ get, configurable: true });\n" \
   "      if (!hosts.has(key)) hosts.set(key, Object.create(friend, { persona: is(() => Object.create(friend.persona,\n" \
   "         { m_unGamePlayedAppID: is(() => app) })) }));\n" \
   "      return hosts.get(key);\n" \
   "   };\n" \
   "   /* What's made of them, changed on the way: the invite's friend, and the\n" \
   "      game's picture. */\n" \
   "   const invited = (type, props) => {\n" \
   "      const invite = props && props.gameInvite, friend = props && props.inviter;\n" \
   "      if (fromFusebox(invite) && friend && ui && friend !== ui.FriendStore.self && friend.persona && friend.persona.is_ingame)\n" \
   "         return [type, { ...props, inviter: hostOf(friend, invite.appID) }];\n" \
   "      /* An invite's picture is made right before the rest of it. */\n" \
   "      if (props && typeof props.strClassNames === 'string' && 'appID' in props) drawn = props.appID;\n" \
   "      if (props && typeof props.strClassNames === 'string' && (props.appID >= ABSENT || lanGames.has(props.appID))) {\n" \
   "         const src = props.appID >= ABSENT && !otherNames(props.appID) ? '' : wide(props.appID);\n" \
   "         if (src) return ['img', { src, className: props.strClassNames }];\n" \
   "         /* Steam has no picture of these numbers. */\n" \
   "         if (props.appID >= ABSENT) return ['div', { className: props.strClassNames, style: { display: 'none' } }];\n" \
   "      }\n" \
   "      return [type, props];\n" \
   "   };\n" \
   "   /* When it was sent is in its bottom right corner: what it's with, like that in\n" \
   "      the left one, over the picture's blur. */\n" \
   "   const stamped = (type, props) => {\n" \
   "      if (type !== 'div' || !props || props.className !== 'invitedTimeLabel'\n" \
   "            || !(drawn >= ABSENT || lanGames.has(drawn))) return [type, props];\n" \
   "      const style = { insetInlineEnd: 'auto', insetInlineStart: '8px', textAlign: 'start', color: 'rgba(255, 255, 255, 0.4)' };\n" \
   "      return [react.Fragment, { children: [react.createElement(type, { key: 'time', ...props }),\n" \
   "         react.createElement(type, { key: 'fusebox', className: props.className, style }, 'FuseBox')] }];\n" \
   "   };\n" \
   "   let fresh = { app: 0, until: 0 };\n" \
   "   /* The app whose invite is being drawn. */\n" \
   "   let drawn = 0;\n" \
   "   /* A function of Steam's, replaced by one that may leave it to it. */\n" \
   "   const around = (o, name, mine) => {\n" \
   "      const was = Object.getOwnPropertyDescriptor(o, name), f = o[name];\n" \
   "      if (typeof f !== 'function') throw new Error(name);\n" \
   "      Object.defineProperty(o, name, { configurable: true, enumerable: true, writable: true,\n" \
   "         value: function(...args) { return mine.call(this, (...a) => f.apply(this, a), ...args); } });\n" \
   "      undoLan.push(() => {\n" \
   "         if (was) Object.defineProperty(o, name, was);\n" \
   "         else delete o[name];\n" \
   "      });\n" \
   "   };\n" \
   "   const lanStart = () => {\n" \
   "      /* It may come after the rest. */\n" \
   "      ui = ui || window.g_FriendsUIApp;\n" \
   "      if (lanReady || !ui || !ui.FriendStore || !ui.ChatStore || !ui.FriendStore.self) return;\n" \
   "      lanReady = true;\n" \
   "      const fs = ui.FriendStore;\n" \
   "      try {\n" \
   "         let proto = fs.self.persona;\n" \
   "         while (proto && !Object.hasOwn(proto, 'connect_string')) proto = Object.getPrototypeOf(proto);\n" \
   "         const was = proto && Object.getOwnPropertyDescriptor(proto, 'connect_string');\n" \
   "         if (!was || !was.get || !was.configurable) throw new Error('connect_string');\n" \
   "         around(fs, 'InviteToGame', function(steams, friend, app, connect) {\n" \
   "            if (connect !== FUSEBOX) return steams(friend, app, connect);\n" \
   "            const game = lanGame();\n" \
   "            if (game && !numbers.has(friend.accountid)) send('invite ' + game + ' ' + id64(friend.accountid));\n" \
   "         });\n" \
   "         /* Steam's own item, while a session is on. */\n" \
   "         around(fs, 'InviteToLobby', function(steams, friend, app, lobby) {\n" \
   "            const game = heard && app === SPACEWAR && !numbers.has(friend.accountid) && lanGame();\n" \
   "            if (!game) return steams(friend, app, lobby);\n" \
   "            send('invite ' + game + ' ' + id64(friend.accountid));\n" \
   "         });\n" \
   "         Object.defineProperty(proto, 'connect_string', { configurable: true, enumerable: was.enumerable, get() {\n" \
   "            const own = was.get.call(this);\n" \
   "            return !own && heard && this === fs.self.persona && !steamGame() && lanGame() ? FUSEBOX : own;\n" \
   "         } });\n" \
   "         undoLan.push(() => Object.defineProperty(proto, 'connect_string', was));\n" \
   "      } catch (e) { }\n" \
   "      /* Wherever an invite is accepted, it comes to one of these. */\n" \
   "      [[ui, 'OpenURLInBrowser'], [SteamClient.URL, 'ExecuteSteamURL'], [SteamClient.WebChat, 'OpenURLInClient']]\n" \
   "         .forEach(([o, name]) => {\n" \
   "            try { around(o, name, function(steams, url, ...more) { return take(url) ? undefined : steams(url, ...more); }); }\n" \
   "            catch (e) { }\n" \
   "         });\n" \
   "      try {\n" \
   "         around(ui.ChatStore.FriendChatStore, 'GetFriendChat', function(steams, ...args) {\n" \
   "            const chat = steams(...args);\n" \
   "            quietChat(chat);\n" \
   "            return chat;\n" \
   "         });\n" \
   "      } catch (e) { }\n" \
   "      try { ui.ChatStore.GameInviteStore.m_mapGameInvites.forEach(list => list.forEach(markInvite)); } catch (e) { }\n" \
   "      quietChats();\n" \
   "      /* An invite's game is the one it was said to be to, where that's here. */\n" \
   "      try {\n" \
   "         const invites = ui.ChatStore.GameInviteStore;\n" \
   "         around(invites, 'RegisterGameInvite', function(steams, who, time, app, ...more) {\n" \
   "            const invite = steams(who, time, app, ...more);\n" \
   "            markInvite(invite);\n" \
   "            return invite;\n" \
   "         });\n" \
   "         around(invites, 'RegisterGameInvitesInMessage', function(steams, who, time, text) {\n" \
   "            note(who, time, text);\n" \
   "            const done = steams(who, time, text);\n" \
   "            /* The toast comes next. */\n" \
   "            const invite = String(text).includes('lobbyinvite') && this.m_mapLatestInvite.get(who);\n" \
   "            if (marked(invite)) fresh = { app: gameOf(invite), until: Date.now() + 5000 };\n" \
   "            return done;\n" \
   "         });\n" \
   "         around(invites, 'GetPendingInvite', function(steams, who) {\n" \
   "            const own = steams(who), invite = own ? null : this.m_mapLatestInvite.get(who);\n" \
   "            if (!fromFusebox(invite) || invite.bDismissed || invite.bAccepted || invite.bOld) return own;\n" \
   "            const friend = fs.GetPlayer(who);\n" \
   "            return friend && friend.persona && friend.persona.is_ingame ? invite : own;\n" \
   "         });\n" \
   "         around(ui.AppInfoStore, 'GetAppInfo', function(steams, app) {\n" \
   "            if (app === SPACEWAR && fresh.app && Date.now() < fresh.until) return infoOf(fresh.app, steams(SPACEWAR));\n" \
   "            if (!lanGames.has(app) && !(app >= ABSENT)) return steams(app);\n" \
   "            return infoOf(app, steams(SPACEWAR));\n" \
   "         });\n" \
   "      } catch (e) { }\n" \
   "   };\n" \
   "   /* Bars drawn before, drawn again. */\n" \
   "   const bars = () => mine && SteamUIStore.WindowStore.SteamUIWindows.forEach(w => {\n" \
   "      try {\n" \
   "         w.BrowserWindow.document.querySelectorAll('.' + CSS.escape(bar.AppButtonsContainer)).forEach(el => {\n" \
   "            let f = el[Object.keys(el).find(k => k.startsWith('__reactFiber'))];\n" \
   "            while (f && !(f.stateNode && f.stateNode.forceUpdate)) f = f.return;\n" \
   "            if (f) f.stateNode.forceUpdate();\n" \
   "            /* And the page it's on, to have it in pages. */\n" \
   "            while (f && !(f.stateNode && f.stateNode.GetSections)) f = f.return;\n" \
   "            if (f) f.stateNode.forceUpdate();\n" \
   "         });\n" \
   "      } catch (e) { }\n" \
   "   });\n" \
   "   window.__fusebox = {\n" \
   "      me,\n" \
   "      version: " STEAMUI_VERSION ",\n" \
   "      opens(list) {\n" \
   "         opens.clear();\n" \
   "         Object.keys(list).forEach(app => opens.set(Number(app), list[app]));\n" \
   "         redraw();\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      lan(apps, others) {\n" \
   "         lanGames.clear();\n" \
   "         apps.forEach(([app, ...names]) => lanGames.set(app, names));\n" \
   "         /* Whatever the library says of them: they run. */\n" \
   "         onces.forEach((names, app) => { if (!lanGames.has(app)) lanGames.set(app, names); });\n" \
   "         inLibrary.clear();\n" \
   "         others.forEach(([id, ...names]) => inLibrary.set(id, names));\n" \
   "         lanStart();\n" \
   "         return lanReady ? 'ok' : 'later';\n" \
   "      },\n" \
   "      /* The pictures of one that isn't in Steam. */\n" \
   "      art(id, icon, wide) {\n" \
   "         art.set(id, { icon, wide });\n" \
   "         new Set(told.map(t => t.who)).forEach(again);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      /* Says what the invite that follows is to. */\n" \
   "      invited(friend, name) {\n" \
   "         if (!ui || !ui.ChatStore) return '';\n" \
   "         const text = 'Invited you to play ' + name + ' with FuseBox';\n" \
   "         ui.ChatStore.GetFriendChat(Number(BigInt(friend) & 0xffffffffn)).SendChatMessage(text);\n" \
   "         note(ui.FriendStore.self.accountid, ui.GetServerRTime32(), text);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      /* Runs a shortcut there is; '' if Steam hasn't it. */\n" \
   "      async run(app) {\n" \
   "         for (let i = 0; i < 100 && !overview(app); i++) await new Promise(f => setTimeout(f, i < 20 ? 20 : 50));\n" \
   "         if (!overview(app)) return '';\n" \
   "         SteamClient.Apps.RunGame(String((BigInt(app) << 32n) | 0x2000000n), '', -1, 100);\n" \
   "         return String(app);\n" \
   "      },\n" \
   "      /* Makes one, and runs it: its app id. */\n" \
   "      async once(name, exe, dir, options, icon, overlay, input) {\n" \
   "         const apps = SteamClient.Apps, app = await apps.AddShortcut(name, exe, '', '');\n" \
   "         if (!app) return '';\n" \
   "         apps.SetShortcutName(app, name);\n" \
   "         apps.SetShortcutExe(app, '\"' + exe + '\"');\n" \
   "         apps.SetShortcutStartDir(app, '\"' + dir + '\"');\n" \
   "         apps.SetShortcutLaunchOptions(app, options);\n" \
   "         if (icon) apps.SetShortcutIcon(app, icon);\n" \
   "         if (!overlay) apps.ToggleEnableSteamOverlayForApp(app);\n" \
   "         if (!input) apps.ToggleAllowDesktopConfiguration(app);\n" \
   "         onces.set(app, [name]);\n" \
   "         lanGames.set(app, [name]);\n" \
   "         lanStart();\n" \
   "         if (await window.__fusebox.run(app)) return String(app);\n" \
   "         window.__fusebox.unonce(app);\n" \
   "         return '';\n" \
   "      },\n" \
   "      unonce(app) {\n" \
   "         onces.delete(app);\n" \
   "         lanGames.delete(app);\n" \
   "         if (games.has(app)) drop(app);\n" \
   "         forget(app);\n" \
   "         SteamClient.Apps.RemoveShortcut(app);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      /* An invite that isn't FuseBox's: Steam's after all. */\n" \
   "      pass(lobby, host) {\n" \
   "         passed.add(lobby);\n" \
   "         SteamClient.URL.ExecuteSteamURL('steam://joinlobby/480/' + lobby + '/' + host);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      /* The next click, '' for none in that time. */\n" \
   "      asked(ms) {\n" \
   "         const was = heard;\n" \
   "         heard = true;\n" \
   "         clearTimeout(deaf);\n" \
   "         deaf = setTimeout(() => { heard = false; redraw(); }, ms + 15000);\n" \
   "         if (!was) redraw();\n" \
   "         if (wake) wake('');\n" \
   "         if (clicks.length) return Promise.resolve(clicks.shift());\n" \
   "         return new Promise(done => {\n" \
   "            const late = setTimeout(() => wake(''), ms);\n" \
   "            wake = app => { clearTimeout(late); wake = null; done(app); };\n" \
   "         });\n" \
   "      },\n" \
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
   "            details(p);\n" \
   "            theirs(app, p);\n" \
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
   "         if (given.has(app) && given.get(app).missing) build(app);\n" \
   "         /* Their pictures may be new. */\n" \
   "         redraw();\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      activity(app, id, wins, added, played) {\n" \
   "         const key = JSON.stringify([id, wins, added, played, games.has(app)]), was = given.get(app);\n" \
   "         const o = appStore.GetAppOverviewByAppID(app);\n" \
   "         const last = () => String(o ? Math.max(o.rt_last_time_played || 0, o.rt_last_time_locally_played || 0) : 0);\n" \
   "         if (was && was.key === key && !was.missing) return last();\n" \
   "         given.set(app, { key, id, wins, added, played, missing: false });\n" \
   "         if (games.has(app)) put(app, games.get(app));\n" \
   "         build(app);\n" \
   "         if (Friend) new Set(wins.map(w => w[1]).filter(Boolean)).forEach(name => theirs(app, person(name)));\n" \
   "         redraw();\n" \
   "         return last();\n" \
   "      },\n" \
   "      /* The best places first, by how many of the board are behind. */\n" \
   "      boards(app, list) {\n" \
   "         const top = b => Math.min(...b[4].map(p => p[2])) / Math.max(1, b[3]);\n" \
   "         const sorted = [...list].sort((x, y) => top(x) - top(y));\n" \
   "         if (JSON.stringify(sorted) === JSON.stringify(boards.get(app))) return 'ok';\n" \
   "         boards.set(app, sorted);\n" \
   "         redraw();\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      set(app, list) {\n" \
   "         const first = !games.has(app), count = first ? 0 : games.get(app).length;\n" \
   "         games.set(app, list);\n" \
   "         put(app, list);\n" \
   "         if (first) mark(app);\n" \
   "         if (overlay && count !== list.length) recount(app);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      /* Those the user has by now, of the ones that were given. */\n" \
   "      unlocked(app, ids) {\n" \
   "         const list = games.get(app), fresh = (list || []).filter(a => !a[5] && ids.includes(a[0]));\n" \
   "         if (!fresh.length) return 'ok';\n" \
   "         fresh.forEach(a => {\n" \
   "            a[5] = 1;\n" \
   "            lately.set(a[0], Math.floor(Date.now() / 1000));\n" \
   "         });\n" \
   "         put(app, list);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      whole() {\n" \
   "         return [...games.keys()].every(app => names(app).every(queryKey =>\n" \
   "            ours(cache.getQueryCache().find({ queryKey })))) && [...acts].every(app => feed.m_mapAppActivity.has(app));\n" \
   "      },\n" \
   "      keep(apps, active) {\n" \
   "         [...games.keys()].filter(a => !apps.includes(a)).forEach(drop);\n" \
   "         [...given.keys()].filter(a => !active.includes(a)).forEach(forget);\n" \
   "         return 'ok';\n" \
   "      },\n" \
   "      undo() {\n" \
   "         stop();\n" \
   "         if (opened) opened.unregister();\n" \
   "         if (overlay) {\n" \
   "            entry.buttonVisible = buttonWas;\n" \
   "            delete facts.GetAppDetails;\n" \
   "            delete facts.GetAchievements;\n" \
   "         }\n" \
   "         later.forEach(clearTimeout);\n" \
   "         undoFriends();\n" \
   "         undoLan.reverse().forEach(f => { try { f(); } catch (e) { } });\n" \
   "         lanGames.clear();\n" \
   "         clearTimeout(deaf);\n" \
   "         opens.clear();\n" \
   "         boards.clear();\n" \
   "         redraw();\n" \
   "         if (wake) wake('');\n" \
   "         if (mine && jsx.jsxs === mine) jsx.jsxs = jsxs;\n" \
   "         if (pictured && jsx.jsx === pictured) jsx.jsx = plain;\n" \
   "         numbers.forEach(p => cache.removeQueries({ queryKey: ['PlayerLinkDetails', id64(p.n)] }));\n" \
   "         [...given.keys()].forEach(forget);\n" \
   "         if (feed) quiet.forEach(name => { delete feed[name]; });\n" \
   "         [...games.keys()].forEach(drop);\n" \
   "         page.prototype.GetSections = original;\n" \
   "         delete window.__fusebox;\n" \
   "         return 'ok';\n" \
   "      }\n" \
   "   };\n" \
   "   bars();\n" \
   "   return 'ready';\n" \
   "})()"

#endif
