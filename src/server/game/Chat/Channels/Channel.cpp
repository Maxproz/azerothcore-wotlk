/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it and/or modify it under version 2 of the License, or (at your option), any later version.
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */

#include "AccountMgr.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SocialMgr.h"
#include "World.h"

Channel::Channel(std::string const& name, uint32 channelId, uint32 channelDBId, TeamId teamId, bool announce, bool ownership):
    _announce(announce),
    _ownership(ownership),
    _IsSaved(false),
    _isOwnerGM(false),
    _flags(0),
    _channelId(channelId),
    _channelDBId(channelDBId),
    _teamId(teamId),
    _name(name),
    _password("")
{
    // set special flags if built-in channel
    if (ChatChannelsEntry const* ch = sChatChannelsStore.LookupEntry(channelId)) // check whether it's a built-in channel
    {
        _announce = false;                                 // no join/leave announces
        _ownership = false;                                // no ownership handout

        _flags |= CHANNEL_FLAG_GENERAL;                    // for all built-in channels

        if (ch->flags & CHANNEL_DBC_FLAG_TRADE)             // for trade channel
            _flags |= CHANNEL_FLAG_TRADE;

        if (ch->flags & CHANNEL_DBC_FLAG_CITY_ONLY2)        // for city only channels
            _flags |= CHANNEL_FLAG_CITY;

        if (ch->flags & CHANNEL_DBC_FLAG_LFG)               // for LFG channel
            _flags |= CHANNEL_FLAG_LFG;
        else                                                // for all other channels
            _flags |= CHANNEL_FLAG_NOT_LFG;
    }
    else                                                    // it's custom channel
    {
        _flags |= CHANNEL_FLAG_CUSTOM;

        // pussywizard:
        _channelRights = ChannelMgr::GetChannelRightsFor(_name);
        if (_channelRights.flags & CHANNEL_RIGHT_FORCE_NO_ANNOUNCEMENTS)
            _announce = false;
        if (_channelRights.flags & CHANNEL_RIGHT_FORCE_ANNOUNCEMENTS)
            _announce = true;
        if (_channelRights.flags & CHANNEL_RIGHT_NO_OWNERSHIP)
            _ownership = false;
        if (_channelRights.flags & CHANNEL_RIGHT_DONT_PRESERVE)
            return;

        _IsSaved = true;

        // Xinef: loading
        if (channelDBId > 0)
            return;

        // If storing custom channels in the db is enabled either load or save the channel
        if (sWorld->getBoolConfig(CONFIG_PRESERVE_CUSTOM_CHANNELS))
        {
            _channelDBId = ++ChannelMgr::_channelIdMax;

            PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHANNEL);
            stmt->setUInt32(0, _channelDBId);
            stmt->setString(1, name);
            stmt->setUInt32(2, _teamId);
            stmt->setUInt8(3, _announce);
            CharacterDatabase.Execute(stmt);
        }
    }
}

bool Channel::IsBanned(ObjectGuid guid) const
{
    BannedContainer::const_iterator itr = bannedStore.find(guid);
    return itr != bannedStore.end() && itr->second > time(nullptr);
}

void Channel::UpdateChannelInDB() const
{
    if (_IsSaved)
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL);
        stmt->setBool(0, _announce);
        stmt->setString(1, _password);
        stmt->setUInt32(2, _channelDBId);
        CharacterDatabase.Execute(stmt);

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
        LOG_DEBUG("chat.system", "Channel(%s) updated in database", _name.c_str());
#endif
    }
}

void Channel::UpdateChannelUseageInDB() const
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL_USAGE);
    stmt->setUInt32(0, _channelDBId);
    CharacterDatabase.Execute(stmt);
}

void Channel::AddChannelBanToDB(ObjectGuid guid, uint32 time) const
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHANNEL_BAN);
    stmt->setUInt32(0, _channelDBId);
    stmt->setUInt32(1, guid.GetCounter());
    stmt->setUInt32(2, time);
    CharacterDatabase.Execute(stmt);
}

void Channel::RemoveChannelBanFromDB(ObjectGuid guid) const
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHANNEL_BAN);
    stmt->setUInt32(0, _channelDBId);
    stmt->setUInt32(1, guid.GetCounter());
    CharacterDatabase.Execute(stmt);
}

void Channel::CleanOldChannelsInDB()
{
    if (sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_DURATION) > 0)
    {
        SQLTransaction trans = CharacterDatabase.BeginTransaction();

        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_OLD_CHANNELS);
        stmt->setUInt32(0, sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_DURATION) * DAY);
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_OLD_CHANNELS_BANS);
        trans->Append(stmt);

        CharacterDatabase.CommitTransaction(trans);
    }
}

void Channel::JoinChannel(Player* player, std::string const& pass)
{
    ObjectGuid guid = player->GetGUID();
    if (IsOn(guid))
    {
        // Do not send error message for built-in channels
        if (!IsConstant())
        {
            WorldPacket data;
            MakePlayerAlreadyMember(&data, guid);
            SendToOne(&data, guid);
        }
        return;
    }

    if (IsBanned(guid))
    {
        WorldPacket data;
        MakeBanned(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!_password.empty() && pass != _password)
    {
        WorldPacket data;
        MakeWrongPassword(&data);
        SendToOne(&data, guid);
        return;
    }

    if (HasFlag(CHANNEL_FLAG_LFG) &&
            sWorld->getBoolConfig(CONFIG_RESTRICTED_LFG_CHANNEL) &&
            AccountMgr::IsPlayerAccount(player->GetSession()->GetSecurity()) &&
            player->GetGroup())
    {
        WorldPacket data;
        MakeNotInLfg(&data);
        SendToOne(&data, guid);
        return;
    }

    player->JoinedChannel(this);

    if (_announce && (!AccountMgr::IsGMAccount(player->GetSession()->GetSecurity()) ||
                      !sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL)))
    {
        WorldPacket data;
        MakeJoined(&data, guid);
        SendToAll(&data);
    }

    PlayerInfo pinfo;
    pinfo.player = guid;
    pinfo.flags = MEMBER_FLAG_NONE;
    pinfo.lastSpeakTime = 0;
    pinfo.plrPtr = player;

    playersStore[guid] = pinfo;

    if (_channelRights.joinMessage.length())
        ChatHandler(player->GetSession()).PSendSysMessage("%s", _channelRights.joinMessage.c_str());

    WorldPacket data;
    MakeYouJoined(&data);
    SendToOne(&data, guid);

    JoinNotify(player);

    playersStore[guid].SetOwnerGM(AccountMgr::IsGMAccount(player->GetSession()->GetSecurity()));

    // Custom channel handling
    if (!IsConstant())
    {
        // Update last_used timestamp in db
        if (!playersStore.empty())
            UpdateChannelUseageInDB();

        if (_channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end())
        {
            playersStore[guid].SetModerator(true);
            FlagsNotify(player);
        }

        // If the channel has no owner yet and ownership is allowed, set the new owner.
        // If the channel owner is a GM and the config SilentGMJoinChannel is enabled, set the new owner
        if ((!_ownerGUID || (_isOwnerGM && sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL))) && _ownership)
        {
            _isOwnerGM = playersStore[guid].IsOwnerGM();
            SetOwner(guid, false);
        }

        if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
            playersStore[guid].SetMuted(true);
    }
}

void Channel::LeaveChannel(Player* player, bool send)
{
    ObjectGuid guid = player->GetGUID();
    if (!IsOn(guid))
    {
        if (send)
        {
            WorldPacket data;
            MakeNotMember(&data);
            SendToOne(&data, guid);
        }
        return;
    }

    if (send)
    {
        WorldPacket data;
        MakeYouLeft(&data);
        SendToOne(&data, guid);
        player->LeftChannel(this);
        data.clear();
    }

    bool changeowner = playersStore[guid].IsOwner();

    playersStore.erase(guid);
    if (_announce && (!AccountMgr::IsGMAccount(player->GetSession()->GetSecurity()) ||
                      !sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL)))
    {
        WorldPacket data;
        MakeLeft(&data, guid);
        SendToAll(&data);
    }

    RemoveWatching(player);
    LeaveNotify(player);

    if (!IsConstant())
    {
        // Update last_used timestamp in db
        UpdateChannelUseageInDB();

        // If the channel owner left and there are still playersStore inside, pick a new owner
        if (changeowner && _ownership)
        {
            if (!playersStore.empty())
            {
                ObjectGuid newowner;
                for (Channel::PlayerContainer::const_iterator itr = playersStore.begin(); itr != playersStore.end(); ++itr)
                {
                    newowner = itr->second.player;
                    if (AccountMgr::IsGMAccount(itr->second.plrPtr->GetSession()->GetSecurity()))
                        _isOwnerGM = true;
                    else
                        _isOwnerGM = false;
                    if (!itr->second.plrPtr->GetSession()->GetSecurity())
                        break;
                }
                SetOwner(newowner);
                // if the new owner is invisible gm, set flag to automatically choose a new owner
            }
            else
                SetOwner(ObjectGuid::Empty);
        }
    }
}

void Channel::KickOrBan(Player const* player, std::string const& badname, bool ban)
{
    AccountTypes sec = player->GetSession()->GetSecurity();
    ObjectGuid good = player->GetGUID();

    if (!IsOn(good))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, good);
        return;
    }

    if (!playersStore[good].IsModerator() && !AccountMgr::IsGMAccount(sec))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, good);
        return;
    }

    bool banOffline = false; // pussywizard
    bool isGoodConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();

    ObjectGuid victim;
    uint32 badAccId = 0;
    uint32 badSecurity = 0;
    Player* bad = ObjectAccessor::FindPlayerByName(badname, false);
    if (bad)
    {
        victim = bad->GetGUID();
        badAccId = bad->GetSession()->GetAccountId();
        badSecurity = bad->GetSession()->GetSecurity();
    }

    bool isOnChannel = victim && IsOn(victim);
    if (!isOnChannel)
    {
        if (ban && (AccountMgr::IsGMAccount(sec) || isGoodConstantModerator))
        {
            if (ObjectGuid guid = sWorld->GetGlobalPlayerGUID(badname))
                if (const GlobalPlayerData* gpd = sWorld->GetGlobalPlayerData(guid.GetCounter()))
                {
                    if (Player::TeamIdForRace(gpd->race) == Player::TeamIdForRace(player->getRace()))
                    {
                        banOffline = true;
                        victim = guid;
                        badAccId = gpd->accountId;
                    }
                    else
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage("Character %s has other faction!", badname.c_str());
                        return;
                    }
                }

            if (!banOffline)
            {
                WorldPacket data;
                MakePlayerNotFound(&data, badname);
                SendToOne(&data, good);
                return;
            }
        }
        else
        {
            WorldPacket data;
            MakePlayerNotFound(&data, badname);
            SendToOne(&data, good);
            return;
        }
    }

    bool changeowner = _ownerGUID == victim;
    bool isBadConstantModerator = _channelRights.moderators.find(badAccId) != _channelRights.moderators.end();

    if (!AccountMgr::IsGMAccount(sec) && !isGoodConstantModerator)
    {
        if (changeowner && good != _ownerGUID)
        {
            WorldPacket data;
            MakeNotOwner(&data);
            SendToOne(&data, good);
            return;
        }

        if ((ban && (_channelRights.flags & CHANNEL_RIGHT_CANT_BAN)) || (!ban && (_channelRights.flags & CHANNEL_RIGHT_CANT_KICK)))
        {
            WorldPacket data;
            MakeNotModerator(&data);
            SendToOne(&data, good);
            return;
        }

        if (isBadConstantModerator || AccountMgr::IsGMAccount(badSecurity))
        {
            WorldPacket data;
            MakeNotModerator(&data);
            SendToOne(&data, good);
            return;
        }
    }

    bool notify = !(AccountMgr::IsGMAccount(sec) && sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL));

    if (ban)
    {
        if (!IsBanned(victim))
        {
            bannedStore[victim] = time(nullptr) + CHANNEL_BAN_DURATION;
            AddChannelBanToDB(victim, time(nullptr) + CHANNEL_BAN_DURATION);

            if (notify)
            {
                WorldPacket data;
                MakePlayerBanned(&data, victim, good);
                SendToAll(&data);
            }
        }
    }
    else if (notify)
    {
        WorldPacket data;
        MakePlayerKicked(&data, victim, good);
        SendToAll(&data);
    }

    if (isOnChannel)
    {
        playersStore.erase(victim);
        bad->LeftChannel(this);
        RemoveWatching(bad);
        LeaveNotify(bad);
    }

    if (changeowner && _ownership)
    {
        if (good != victim)
            SetOwner(good);
        else if (!playersStore.empty())
        {
            ObjectGuid newowner;
            for (Channel::PlayerContainer::const_iterator itr = playersStore.begin(); itr != playersStore.end(); ++itr)
            {
                newowner = itr->second.player;
                if (!itr->second.plrPtr->GetSession()->GetSecurity())
                    break;
            }
            SetOwner(newowner);
        }
        else
            SetOwner(ObjectGuid::Empty);
    }
}

void Channel::UnBan(Player const* player, std::string const& badname)
{
    uint32 sec = player->GetSession()->GetSecurity();
    ObjectGuid good = player->GetGUID();

    if (!IsOn(good))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, good);
        return;
    }

    if (!playersStore[good].IsModerator() && !AccountMgr::IsGMAccount(sec))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, good);
        return;
    }

    ObjectGuid victim;
    if (ObjectGuid guid = sWorld->GetGlobalPlayerGUID(badname))
        victim = guid;

    if (!victim || !IsBanned(victim))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, badname);
        SendToOne(&data, good);
        return;
    }

    bool isConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();
    if (!AccountMgr::IsGMAccount(sec) && !isConstantModerator)
    {
        if (_channelRights.flags & CHANNEL_RIGHT_CANT_BAN)
        {
            WorldPacket data;
            MakeNotModerator(&data);
            SendToOne(&data, good);
            return;
        }
    }

    if (_channelRights.flags & CHANNEL_RIGHT_CANT_BAN)
        LOG_GM(player->GetSession()->GetAccountId(), "Command: /unban %s %s (Moderator %s [%s, account: %u] unbanned %s [%s])",
            GetName().c_str(), badname.c_str(), player->GetName().c_str(), player->GetGUID().ToString().c_str(), player->GetSession()->GetAccountId(),
            badname.c_str(), victim.ToString().c_str());

    bannedStore.erase(victim);
    RemoveChannelBanFromDB(victim);

    WorldPacket data;
    MakePlayerUnbanned(&data, victim, good);
    SendToAll(&data);
}

void Channel::UnBan(ObjectGuid guid)
{
    if (!IsBanned(guid))
        return;
    bannedStore.erase(guid);
    RemoveChannelBanFromDB(guid);
}

void Channel::Password(Player const* player, std::string const& pass)
{
    ObjectGuid guid = player->GetGUID();

    ChatHandler chat(player->GetSession());
    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && !AccountMgr::IsGMAccount(player->GetSession()->GetSecurity()))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (_channelRights.flags & CHANNEL_RIGHT_CANT_CHANGE_PASSWORD)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    _password = pass;

    WorldPacket data;
    MakePasswordChanged(&data, guid);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::SetMode(Player const* player, std::string const& p2n, bool mod, bool set)
{
    ObjectGuid guid = player->GetGUID();
    uint32 sec = player->GetSession()->GetSecurity();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && !AccountMgr::IsGMAccount(sec))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (guid == _ownerGUID && std::string(p2n) == player->GetName() && mod)
        return;

    Player* newp = ObjectAccessor::FindPlayerByName(p2n, false);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!victim || !IsOn(victim) ||
            // allow make moderator from another team only if both is GMs
            // at this moment this only way to show channel post for GM from another team
            ((!AccountMgr::IsGMAccount(sec) || !AccountMgr::IsGMAccount(newp->GetSession()->GetSecurity())) && player->GetTeamId() != newp->GetTeamId() &&
             !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL)))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, p2n);
        SendToOne(&data, guid);
        return;
    }

    if (_ownerGUID == victim && _ownerGUID != guid)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, guid);
        return;
    }

    if (mod)
    {
        bool isBadConstantModerator = _channelRights.moderators.find(newp->GetSession()->GetAccountId()) != _channelRights.moderators.end();
        if (!isBadConstantModerator)
            SetModerator(newp->GetGUID(), set);
    }
    else
    {
        bool isGoodConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();
        if (!AccountMgr::IsGMAccount(sec) && !isGoodConstantModerator)
        {
            if (_channelRights.flags & CHANNEL_RIGHT_CANT_MUTE)
            {
                WorldPacket data;
                MakeNotModerator(&data);
                SendToOne(&data, guid);
                return;
            }
        }

        SetMute(newp->GetGUID(), set);
    }
}

void Channel::SetOwner(Player const* player, std::string const& newname)
{
    ObjectGuid guid = player->GetGUID();
    uint32 sec = player->GetSession()->GetSecurity();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    bool isGoodConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();
    if (!AccountMgr::IsGMAccount(sec) && guid != _ownerGUID && !isGoodConstantModerator)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindPlayerByName(newname, false);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!victim || !IsOn(victim) || (newp->GetTeamId() != player->GetTeamId() &&
                                     !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL)))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    SetOwner(victim);
}

void Channel::SendWhoOwner(ObjectGuid guid)
{
    WorldPacket data;
    if (IsOn(guid))
        MakeChannelOwner(&data);
    else
        MakeNotMember(&data);
    SendToOne(&data, guid);
}

void Channel::List(Player const* player)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    LOG_DEBUG("chat.system", "SMSG_CHANNEL_LIST %s Channel: %s", player->GetSession()->GetPlayerInfo().c_str(), GetName().c_str());
#endif
    WorldPacket data(SMSG_CHANNEL_LIST, 1 + (GetName().size() + 1) + 1 + 4 + playersStore.size() * (8 + 1));
    data << uint8(1);                                   // channel type?
    data << GetName();                                  // channel name
    data << uint8(GetFlags());                          // channel flags?

    size_t pos = data.wpos();
    data << uint32(0);                                  // size of list, placeholder

    uint32 count  = 0;
    if (!(_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK))
        for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
            if (AccountMgr::IsPlayerAccount(i->second.plrPtr->GetSession()->GetSecurity()))
            {
                data << i->first;
                data << uint8(i->second.flags); // flags seems to be changed...
                ++count;
            }

    data.put<uint32>(pos, count);

    SendToOne(&data, guid);
}

void Channel::Announce(Player const* player)
{
    ObjectGuid guid = player->GetGUID();
    uint32 sec = player->GetSession()->GetSecurity();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && !AccountMgr::IsGMAccount(sec))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (_channelRights.flags & (CHANNEL_RIGHT_FORCE_NO_ANNOUNCEMENTS | CHANNEL_RIGHT_FORCE_ANNOUNCEMENTS))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    _announce = !_announce;

    WorldPacket data;
    if (_announce)
        MakeAnnouncementsOn(&data, guid);
    else
        MakeAnnouncementsOff(&data, guid);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::Say(ObjectGuid guid, std::string const& what, uint32 lang)
{
    if (what.empty())
        return;

    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
        lang = LANG_UNIVERSAL;

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    PlayerInfo& pinfo = playersStore[guid];

    if (pinfo.IsMuted())
    {
        WorldPacket data;
        MakeMuted(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* player = pinfo.plrPtr;

    if (player && player->GetSession()->GetSecurity() == AccountTypes::SEC_PLAYER) // pussywizard: prevent spam on populated channels
    {
        uint32 speakDelay = 0;
        if (_channelRights.speakDelay > 0)
            speakDelay = _channelRights.speakDelay;
        else if (playersStore.size() >= 10)
            speakDelay = 5;

        if (!pinfo.IsAllowedToSpeak(speakDelay))
        {
            std::string timeStr = secsToTimeString(pinfo.lastSpeakTime + speakDelay - sWorld->GetGameTime());
            if (_channelRights.speakMessage.length() > 0)
                player->GetSession()->SendNotification("%s", _channelRights.speakMessage.c_str());
            player->GetSession()->SendNotification("You must wait %s before speaking again.", timeStr.c_str());
            return;
        }
    }

    WorldPacket data;
    if (player)
        ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, Language(lang), player, player, what, 0, _name);
    else
        ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, Language(lang), guid, guid, what, 0, "", "", 0, false, _name);

    SendToAll(&data, pinfo.IsModerator() ? ObjectGuid::Empty : guid);
}

void Channel::EveryoneSayToSelf(const char* what)
{
    if (!what)
        return;

    uint32 messageLength = strlen(what) + 1;

    WorldPacket data(SMSG_MESSAGECHAT, 1 + 4 + 8 + 4 + _name.size() + 1 + 8 + 4 + messageLength + 1);
    data << (uint8)CHAT_MSG_CHANNEL;
    data << (uint32)LANG_UNIVERSAL;
    data << uint64(0); // put player guid here
    data << uint32(0);
    data << _name;
    data << uint64(0); // put player guid here
    data << messageLength;
    data << what;
    data << uint8(0);

    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
    {
        data.put(5, i->first);
        data.put(17 + _name.size() + 1, i->first);
        i->second.plrPtr->GetSession()->SendPacket(&data);
    }
}

void Channel::Invite(Player const* player, std::string const& newname)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindPlayerByName(newname, false);
    if (!newp || !newp->isGMVisible())
    {
        WorldPacket data;
        MakePlayerNotFound(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    if (IsBanned(newp->GetGUID()))
    {
        WorldPacket data;
        MakePlayerInviteBanned(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    if (newp->GetTeamId() != player->GetTeamId())
    {
        WorldPacket data;
        MakeInviteWrongFaction(&data);
        SendToOne(&data, guid);
        return;
    }

    if (IsOn(newp->GetGUID()))
    {
        WorldPacket data;
        MakePlayerAlreadyMember(&data, newp->GetGUID());
        SendToOne(&data, guid);
        return;
    }

    if (!newp->GetSocial()->HasIgnore(guid))
    {
        WorldPacket data;
        MakeInvite(&data, guid);
        SendToOne(&data, newp->GetGUID());
        data.clear();
    }

    WorldPacket data;
    MakePlayerInvited(&data, newp->GetName());
    SendToOne(&data, guid);
}

void Channel::SetOwner(ObjectGuid guid, bool exclaim)
{
    if (_ownerGUID)
    {
        // [] will re-add player after it possible removed
        PlayerContainer::iterator p_itr = playersStore.find(_ownerGUID);
        if (p_itr != playersStore.end())
        {
            p_itr->second.SetOwner(false);
            FlagsNotify(p_itr->second.plrPtr);
        }
    }

    _ownerGUID = guid;
    if (_ownerGUID)
    {
        PlayerInfo& pinfo = playersStore[_ownerGUID];

        pinfo.SetModerator(true);
        uint8 oldFlag = pinfo.flags;
        pinfo.SetOwner(true);

        bool notify = true;
        Player* player = ObjectAccessor::FindPlayer(_ownerGUID);

        if (player)
        {
            uint32 sec = player->GetSession()->GetSecurity();
            notify = !(AccountMgr::IsGMAccount(sec) && sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL));
        }

        WorldPacket data;

        if (notify)
        {
            MakeModeChange(&data, _ownerGUID, oldFlag);
            SendToAll(&data);
        }

        FlagsNotify(pinfo.plrPtr);

        if (exclaim && notify)
        {
            MakeOwnerChanged(&data, _ownerGUID);
            SendToAll(&data);
        }
    }
}

void Channel::SendToAll(WorldPacket* data, ObjectGuid guid)
{
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
        if (!guid || !i->second.plrPtr->GetSocial()->HasIgnore(guid))
            i->second.plrPtr->GetSession()->SendPacket(data);
}

void Channel::SendToAllButOne(WorldPacket* data, ObjectGuid who)
{
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
        if (i->first != who)
            i->second.plrPtr->GetSession()->SendPacket(data);
}

void Channel::SendToOne(WorldPacket* data, ObjectGuid who)
{
    if (Player* player = ObjectAccessor::FindConnectedPlayer(who))
        player->GetSession()->SendPacket(data);
}

void Channel::SendToAllWatching(WorldPacket* data)
{
    for (PlayersWatchingContainer::const_iterator i = playersWatchingStore.begin(); i != playersWatchingStore.end(); ++i)
        (*i)->GetSession()->SendPacket(data);
}

void Channel::Voice(ObjectGuid /*guid1*/, ObjectGuid /*guid2*/)
{
}

void Channel::DeVoice(ObjectGuid /*guid1*/, ObjectGuid /*guid2*/)
{
}

void Channel::MakeNotifyPacket(WorldPacket* data, uint8 notify_type)
{
    data->Initialize(SMSG_CHANNEL_NOTIFY, 1 + _name.size());
    *data << uint8(notify_type);
    *data << _name;
}

void Channel::MakeJoined(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_JOINED_NOTICE);
    *data << guid;
}

void Channel::MakeLeft(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_LEFT_NOTICE);
    *data << guid;
}

void Channel::MakeYouJoined(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_YOU_JOINED_NOTICE);
    *data << uint8(GetFlags());
    *data << uint32(GetChannelId());
    *data << uint32(0);
}

void Channel::MakeYouLeft(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_YOU_LEFT_NOTICE);
    *data << uint32(GetChannelId());
    *data << uint8(IsConstant());
}

void Channel::MakeWrongPassword(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_WRONG_PASSWORD_NOTICE);
}

void Channel::MakeNotMember(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MEMBER_NOTICE);
}

void Channel::MakeNotModerator(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MODERATOR_NOTICE);
}

void Channel::MakePasswordChanged(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_PASSWORD_CHANGED_NOTICE);
    *data << guid;
}

void Channel::MakeOwnerChanged(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_OWNER_CHANGED_NOTICE);
    *data << guid;
}

void Channel::MakePlayerNotFound(WorldPacket* data, std::string const& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_NOT_FOUND_NOTICE);
    *data << name;
}

void Channel::MakeNotOwner(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_OWNER_NOTICE);
}

void Channel::MakeChannelOwner(WorldPacket* data)
{
    std::string name = "";

    if (!sObjectMgr->GetPlayerNameByGUID(_ownerGUID.GetCounter(), name) || name.empty())
        name = "PLAYER_NOT_FOUND";

    MakeNotifyPacket(data, CHAT_CHANNEL_OWNER_NOTICE);
    *data << ((IsConstant() || !_ownerGUID) ? "Nobody" : name);
}

void Channel::MakeModeChange(WorldPacket* data, ObjectGuid guid, uint8 oldflags)
{
    MakeNotifyPacket(data, CHAT_MODE_CHANGE_NOTICE);
    *data << guid;
    *data << uint8(oldflags);
    *data << uint8(GetPlayerFlags(guid));
}

void Channel::MakeAnnouncementsOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_ANNOUNCEMENTS_ON_NOTICE);
    *data << guid;
}

void Channel::MakeAnnouncementsOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_ANNOUNCEMENTS_OFF_NOTICE);
    *data << guid;
}

void Channel::MakeMuted(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_MUTED_NOTICE);
}

void Channel::MakePlayerKicked(WorldPacket* data, ObjectGuid bad, ObjectGuid good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_KICKED_NOTICE);
    *data << bad;
    *data << good;
}

void Channel::MakeBanned(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_BANNED_NOTICE);
}

void Channel::MakePlayerBanned(WorldPacket* data, ObjectGuid bad, ObjectGuid good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_BANNED_NOTICE);
    *data << bad;
    *data << good;
}

void Channel::MakePlayerUnbanned(WorldPacket* data, ObjectGuid bad, ObjectGuid good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_UNBANNED_NOTICE);
    *data << bad;
    *data << good;
}

void Channel::MakePlayerNotBanned(WorldPacket* data, const std::string& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_NOT_BANNED_NOTICE);
    *data << name;
}

void Channel::MakePlayerAlreadyMember(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_PLAYER_ALREADY_MEMBER_NOTICE);
    *data << guid;
}

void Channel::MakeInvite(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_INVITE_NOTICE);
    *data << guid;
}

void Channel::MakeInviteWrongFaction(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_INVITE_WRONG_FACTION_NOTICE);
}

void Channel::MakeWrongFaction(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_WRONG_FACTION_NOTICE);
}

void Channel::MakeInvalidName(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_INVALID_NAME_NOTICE);
}

void Channel::MakeNotModerated(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MODERATED_NOTICE);
}

void Channel::MakePlayerInvited(WorldPacket* data, const std::string& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_INVITED_NOTICE);
    *data << name;
}

void Channel::MakePlayerInviteBanned(WorldPacket* data, const std::string& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_INVITE_BANNED_NOTICE);
    *data << name;
}

void Channel::MakeThrottled(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_THROTTLED_NOTICE);
}

void Channel::MakeNotInArea(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_IN_AREA_NOTICE);
}

void Channel::MakeNotInLfg(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_IN_LFG_NOTICE);
}

void Channel::MakeVoiceOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_VOICE_ON_NOTICE);
    *data << guid;
}

void Channel::MakeVoiceOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_VOICE_OFF_NOTICE);
    *data << guid;
}

void Channel::JoinNotify(Player* p)
{
    if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
        return;
    if (!AccountMgr::IsPlayerAccount(p->GetSession()->GetSecurity()))
        return;

    WorldPacket data(SMSG_USERLIST_ADD, 8 + 1 + 1 + 4 + GetName().size());
    data << p->GetGUID();
    data << uint8(GetPlayerFlags(p->GetGUID()));
    data << uint8(GetFlags());
    data << uint32(GetNumPlayers());
    data << GetName();

    SendToAllWatching(&data);
}

void Channel::LeaveNotify(Player* p)
{
    if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
        return;
    if (!AccountMgr::IsPlayerAccount(p->GetSession()->GetSecurity()))
        return;

    WorldPacket data(SMSG_USERLIST_REMOVE, 8 + 1 + 4 + GetName().size());
    data << p->GetGUID();
    data << uint8(GetFlags());
    data << uint32(GetNumPlayers());
    data << GetName();

    SendToAllWatching(&data);
}

void Channel::FlagsNotify(Player* p)
{
    if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
        return;
    if (!AccountMgr::IsPlayerAccount(p->GetSession()->GetSecurity()))
        return;

    WorldPacket data(SMSG_USERLIST_UPDATE, 8 + 1 + 1 + 4 + GetName().size());
    data << p->GetGUID();
    data << uint8(GetPlayerFlags(p->GetGUID()));
    data << uint8(GetFlags());
    data << uint32(GetNumPlayers());
    data << GetName();

    SendToAllWatching(&data);
}

void Channel::AddWatching(Player* p)
{
    if (!IsOn(p->GetGUID()))
        return;
    playersWatchingStore.insert(p);
}

void Channel::RemoveWatching(Player* p)
{
    PlayersWatchingContainer::iterator itr = playersWatchingStore.find(p);
    if (itr != playersWatchingStore.end())
        playersWatchingStore.erase(itr);
}

void Channel::ToggleModeration(Player* player)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    const uint32 level = sWorld->getIntConfig(CONFIG_GM_LEVEL_CHANNEL_MODERATION);
    const bool gm = (level && player->GetSession()->GetSecurity() >= level);

    if (!playersStore[guid].IsModerator() && !gm)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    // toggle channel moderation
    _moderation = !_moderation;

    WorldPacket data;
    if (_moderation)
    {
        MakeModerationOn(&data, guid);
    }
    else
    {
        MakeModerationOff(&data, guid);
    }

    SendToAll(&data);
}

void Channel::MakeModerationOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_MODERATION_ON_NOTICE);
    *data << guid;
}

void Channel::MakeModerationOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_MODERATION_OFF_NOTICE);
    *data << guid;
}
