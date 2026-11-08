Fixes in this build
-------------------
1. Upstream syntax error: if (loose_game_active) if (loose_game_active)
2. Windows Unicode crash: no path::string() while enumerating disc files
3. Virtual INFO.DAT/GAME.DAT no longer require host files (was throwing
   "missing or non-file extracted path")
4. Rebuild INFO.DAT from GAME/ (Arika format from arika_tooling_sdbz)
5. Map all GAME/ files onto virtual GAME.DAT

Expected log without host INFO.DAT:
  [disc] rebuilt INFO.DAT from GAME/ (N assets, M sectors)
  [disc] synthesized ISO directory and file extents in memory
  [disc] INFO.DAT is virtual; host-INFO overlay pass skipped
