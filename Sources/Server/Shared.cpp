/*
 Copyright (c) 2013 yvt
 based on code of pysnip (c) Mathias Kaerlev 2011-2012.
 
 This file is part of OpenSpades.
 
 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 
 */

#include "Shared.h"
#include <Core/ENetTools.h>
#include <Core/Debug.h>
#include <Core/VersionInfo.h>
#include <OpenSpades.h>
#include <Core//Settings.h>

SPADES_SETTING(core_locale, "");

namespace spades { namespace protocol {
	
	typedef Packet *(*PacketDecodeFuncType)(const std::vector<char>&);
	
	class PacketTypeFinder {
		PacketType type;
	public:
		constexpr PacketTypeFinder(PacketType type): type(type) {}
		template<class T> inline constexpr bool evaluate() const { return type == T::Type; }
	};
	class PacketTypeToDecoder {
	public:
		template<class T> inline constexpr PacketDecodeFuncType evaluate() const { return &T::Decode; }
		inline constexpr PacketDecodeFuncType not_found() const { return nullptr; }
	};
	
	
	class PacketDecodeTableGenerator {
	public:
		constexpr PacketDecodeFuncType operator [](std::size_t index) const {
			return stmp::find_type_list<PacketTypeFinder, PacketTypeToDecoder, PacketClassList>
			(PacketTypeFinder(static_cast<PacketType>(index)), PacketTypeToDecoder()).evaluate();
		}
	};
	
	static constexpr auto packetDecodeTable = stmp::make_static_table<128>(PacketDecodeTableGenerator());
	
	class PacketReader: public NetPacketReader {
	public:
		PacketReader(const std::vector<char>& bytes):
		NetPacketReader(bytes) {}
		
		uint64_t ReadVariableInteger() {
			SPADES_MARK_FUNCTION();
			
			uint32_t v = 0;
			int shift = 0;
			while(true) {
				uint8_t b = ReadByte();
				v |= static_cast<uint32_t>(b & 0x7f) << shift;
				if(b & 0x80) {
					shift += 7;
				}else{
					break;
				}
			}
			return v;
		}
		
		std::string ReadBytes(){
			SPADES_MARK_FUNCTION();
			
			auto len = ReadVariableInteger();
			if(len > 1024 * 1024) {
				SPRaise("String too long.: %llu",
						static_cast<unsigned long long>(len));
			}
			
			// convert to C string once so that
			// null-chars are removed
			std::string s = ReadData(static_cast<std::size_t>(len)).c_str();
			return s;
		}
		
		std::string ReadString(){
			return ReadBytes();
		}
		
		template<class T>
		T ReadMap() {
			T dict;
			while(true){
				auto key = ReadString();
				if(key.empty()) break;
				auto value = ReadString();
				dict.insert(std::make_pair(key, value));
			}
			return std::move(dict);
		}
		
		TimeStampType ReadTimeStamp() {
			return static_cast<TimeStampType>(ReadVariableInteger());
		}
		
	};
	class PacketWriter: public NetPacketWriter {
	public:
		PacketWriter(PacketType type):
		NetPacketWriter(static_cast<unsigned int>(type)) {}
		
		void WriteVariableInteger(uint64_t i) {
			SPADES_MARK_FUNCTION();
			
			while(true) {
				uint8_t b = static_cast<uint8_t>(i & 0x7f);
				i >>= 7;
				if(i) {
					b |= 0x80;
					Write(b);
				}else{
					Write(b);
					break;
				}
			}
		}
		
		void WriteBytes(std::string str){
			SPADES_MARK_FUNCTION();
			
			WriteVariableInteger(str.size());
			Write(str);
		}
		void WriteString(std::string str){
			WriteBytes(str);
		}
		template<class T>
		void WriteMap(const T& dict) {
			for(const auto& item: dict) {
				if(item.first.empty()) continue;
				WriteString(item.first);
				WriteString(item.second);
			}
			WriteString(std::string());
		}
		
		using NetPacketWriter::Write;
		void Write(TimeStampType t) {
			WriteVariableInteger(static_cast<uint64_t>(t));
		}
	};
	
	Packet *Packet::Decode(const std::vector<char>& data) {
		SPADES_MARK_FUNCTION();
		
		if(data.size() == 0) {
			SPRaise("Packet truncated");
		}
		
		auto typeIndex = static_cast<std::size_t>(data[0]);
		if(typeIndex >= packetDecodeTable.size()) {
			return nullptr;
		}
		
		auto *ptr = packetDecodeTable[typeIndex];
		if(ptr == nullptr) {
			return nullptr;
		}
		
		return ptr(data);
	}
	
	
	Packet *GreetingPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<GreetingPacket> p(new GreetingPacket());
		PacketReader reader(data);
		
		auto magic = reader.ReadString();
		if(magic != "Hello") {
			SPRaise("Invalid magic.");
		}
		p->nonce = reader.ReadBytes();
		
		return p.release();
	}
	
	std::vector<char> GreetingPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.WriteString("Hello");
		writer.WriteBytes(nonce);
		
		return std::move(writer.ToArray());
	}
	
	
	InitiateConnectionPacket InitiateConnectionPacket::CreateDefault() {
		SPADES_MARK_FUNCTION();
		
		InitiateConnectionPacket ret;
		ret.protocolName = ProtocolName;
		if(ret.protocolName.size() > 256) ret.protocolName.resize(256);
		ret.majorVersion = OpenSpades_VERSION_MAJOR;
		ret.minorVersion = OpenSpades_VERSION_MINOR;
		ret.revision = OpenSpades_VERSION_REVISION;
		ret.packageString = PACKAGE_STRING;
		if(ret.packageString.size() > 256) ret.packageString.resize(256);
		ret.environmentString = VersionInfo::GetVersionInfo();
		if(ret.environmentString.size() > 1024) ret.environmentString.resize(1024);
		ret.locale = static_cast<std::string>(core_locale);
		if(ret.locale.size() > 256) ret.locale.resize(256);
		
		return ret;
	}
	
	Packet *InitiateConnectionPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<InitiateConnectionPacket> p(new InitiateConnectionPacket());
		PacketReader reader(data);
		
		p->protocolName = reader.ReadString();
		p->majorVersion = reader.ReadShort();
		p->minorVersion = reader.ReadShort();
		p->revision = reader.ReadShort();
		p->packageString = reader.ReadString();
		p->environmentString = reader.ReadString();
		p->locale = reader.ReadString();
		p->playerName = reader.ReadString();
		p->nonce = reader.ReadBytes();
		
		return p.release();
	}
	
	std::vector<char> InitiateConnectionPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.WriteString(protocolName);
		writer.Write(majorVersion);
		writer.Write(minorVersion);
		writer.Write(revision);
		writer.WriteString(packageString);
		writer.WriteString(environmentString);
		writer.WriteString(locale);
		writer.WriteString(playerName);
		writer.WriteBytes(nonce);
		
		return std::move(writer.ToArray());
	}
	
	
	Packet *ServerCertificatePacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<ServerCertificatePacket> p(new ServerCertificatePacket());
		PacketReader reader(data);
		
		p->isValid = reader.ReadByte() != 0;
		
		if(p->isValid) {
			p->certificate = reader.ReadBytes();
			p->signature = reader.ReadBytes();
		}
		
		return p.release();
	}
	
	std::vector<char> ServerCertificatePacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.Write(static_cast<uint8_t>(isValid ? 1 : 0));
		if(isValid) {
			writer.WriteBytes(certificate);
			writer.WriteBytes(signature);
		}
			
		return std::move(writer.ToArray());
	}
	
	
	Packet *ClientCertificatePacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<ClientCertificatePacket> p(new ClientCertificatePacket());
		PacketReader reader(data);
		
		p->isValid = reader.ReadByte() != 0;
		
		if(p->isValid) {
			p->certificate = reader.ReadBytes();
			p->signature = reader.ReadBytes();
		}
		
		return p.release();
	}
	
	std::vector<char> ClientCertificatePacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.Write(static_cast<uint8_t>(isValid ? 1 : 0));
		if(isValid) {
			writer.WriteBytes(certificate);
			writer.WriteBytes(signature);
		}
		
		return std::move(writer.ToArray());
	}
	
	
	Packet *KickPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<KickPacket> p(new KickPacket());
		PacketReader reader(data);
		
		p->reason = reader.ReadString();
		
		return p.release();
	}
	
	std::vector<char> KickPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.WriteString(reason);
		
		return std::move(writer.ToArray());
	}
	
	Packet *GameStateHeaderPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<GameStateHeaderPacket> p(new GameStateHeaderPacket());
		PacketReader reader(data);
		
		p->properties = reader.ReadMap<std::map<std::string, std::string>>();
		
		return p.release();
	}
	
	std::vector<char> GameStateHeaderPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.WriteMap(properties);
		
		return std::move(writer.ToArray());
	}

	
	
	Packet *MapDataPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<MapDataPacket> p(new MapDataPacket());
		PacketReader reader(data);
		
		p->fragment = reader.ReadBytes();
		
		return p.release();
	}
	
	std::vector<char> MapDataPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.WriteBytes(fragment);
		
		return std::move(writer.ToArray());
	}
	
	
	Packet *GameStateFinalPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<GameStateFinalPacket> p(new GameStateFinalPacket());
		PacketReader reader(data);
		
		p->properties = reader.ReadMap<std::map<std::string, std::string>>();
		
		return p.release();
	}
	
	std::vector<char> GameStateFinalPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.WriteMap(properties);
		
		return std::move(writer.ToArray());
	}
	
	enum class EntityUpdateFlags {
		None = 0,
		Create = 1 << 0,
		Flags = 1 << 1,
		Trajectory = 1 << 2,
		PlayerInput = 1 << 3,
		Tool = 1 << 4,
		BlockColor = 1 << 5,
		Health = 1 << 6,
		Skins = 1 << 7
	};
	
	inline EntityUpdateFlags operator|(EntityUpdateFlags a, EntityUpdateFlags b)
	{
		return static_cast<EntityUpdateFlags>(static_cast<int>(a) | static_cast<int>(b));
	}
	inline EntityUpdateFlags& operator |=(EntityUpdateFlags& a, EntityUpdateFlags b) {
		a = a | b;
		return a;
	}
	inline bool operator&(EntityUpdateFlags a, EntityUpdateFlags b)
	{
		return static_cast<int>(a) & static_cast<int>(b);
	}
	
	enum class EntityFlagsValue {
		None = 0,
		PlayerClip = 1 << 0,
		WeaponClip = 1 << 1,
		Fly = 1 << 2
	};
	
	inline EntityFlagsValue operator|(EntityFlagsValue a, EntityFlagsValue b)
	{
		return static_cast<EntityFlagsValue>(static_cast<int>(a) | static_cast<int>(b));
	}
	inline EntityFlagsValue& operator |=(EntityFlagsValue& a, EntityFlagsValue b) {
		a = a | b;
		return a;
	}
	inline bool operator&(EntityFlagsValue a, EntityFlagsValue b)
	{
		return static_cast<int>(a) & static_cast<int>(b);
	}
	
	inline EntityFlagsValue ToEntityFlagsValue(EntityFlags flags) {
		auto ret = EntityFlagsValue::None;
		if(flags.playerClip) ret |= EntityFlagsValue::PlayerClip;
		if(flags.weaponClip) ret |= EntityFlagsValue::WeaponClip;
		if(flags.fly) ret |= EntityFlagsValue::Fly;
		return ret;
	}
	
	inline EntityFlags FromEntityFlagsValue(EntityFlagsValue val) {
		EntityFlags ret;
		ret.playerClip = val & EntityFlagsValue::PlayerClip;
		ret.weaponClip = val & EntityFlagsValue::WeaponClip;
		ret.fly = val & EntityFlagsValue::Fly;
		return ret;
	}
	
	static Trajectory DecodeTrajectory(PacketReader& reader) {
		Trajectory traj;
		traj.type = static_cast<TrajectoryType>(reader.ReadByte());
		traj.origin = reader.ReadVector3();
		traj.velocity = reader.ReadVector3();
		
		switch(traj.type) {
			case game::TrajectoryType::Linear:
			case game::TrajectoryType::Gravity:
			case game::TrajectoryType::Constant:
			case game::TrajectoryType::RigidBody:
				traj.angle = Quaternion::DecodeRotation(reader.ReadVector3());
				traj.angularVelocity = reader.ReadVector3();
				break;
			case game::TrajectoryType::Player:
				traj.eulerAngle = reader.ReadVector3();
				break;
			default:
				SPRaise("Unknown trajectory type: %d",
						static_cast<int>(traj.type));
		}
		return traj;
	}
	
	static void WriteTrajectory(PacketWriter& writer, const Trajectory& traj) {
		writer.Write(static_cast<uint8_t>(traj.type));
		writer.Write(traj.origin);
		writer.Write(traj.velocity);
		switch(traj.type) {
			case game::TrajectoryType::Linear:
			case game::TrajectoryType::Gravity:
			case game::TrajectoryType::Constant:
			case game::TrajectoryType::RigidBody:
				writer.Write(traj.angle.EncodeRotation());
				writer.Write(traj.angularVelocity);
				break;
			case game::TrajectoryType::Player:
				writer.Write(traj.eulerAngle);
				break;
			default:
				SPRaise("Unknown trajectory type: %d",
						static_cast<int>(traj.type));
		}
	}
	
	enum class PlayerInputFlags {
		None = 0,
		ToolPrimary = 1 << 0,
		ToolSecondary = 1 << 1,
		Chat = 1 << 2,
		Sprint = 1 << 3,
		
		StanceMask = 3 << 6
	};
	
	inline PlayerInputFlags operator|(PlayerInputFlags a, PlayerInputFlags b)
	{
		return static_cast<PlayerInputFlags>(static_cast<int>(a) | static_cast<int>(b));
	}
	inline PlayerInputFlags& operator |=(PlayerInputFlags& a, PlayerInputFlags b) {
		a = a | b;
		return a;
	}
	inline int operator&(PlayerInputFlags a, PlayerInputFlags b)
	{
		return static_cast<int>(a) & static_cast<int>(b);
	}
	
	static PlayerInput DecodePlayerInput(PacketReader& reader) {
		PlayerInput inp;
		auto flags = static_cast<PlayerInputFlags>(reader.ReadByte());
		inp.toolPrimary = flags & PlayerInputFlags::ToolPrimary;
		inp.toolSecondary = flags & PlayerInputFlags::ToolSecondary;
		inp.chat = flags & PlayerInputFlags::Chat;
		inp.sprint = flags & PlayerInputFlags::Sprint;
		inp.stance = static_cast<game::PlayerStance>((flags & PlayerInputFlags::StanceMask) >> 6);
		inp.xmove = static_cast<int8_t>(reader.ReadByte());
		inp.ymove = static_cast<int8_t>(reader.ReadByte());
		return inp;
	}
	
	static void WritePlayerInput(PacketWriter& writer, const PlayerInput& input) {
		auto flags = PlayerInputFlags::None;
		if(input.toolPrimary) flags |= PlayerInputFlags::ToolPrimary;
		if(input.toolSecondary) flags |= PlayerInputFlags::ToolSecondary;
		if(input.chat) flags |= PlayerInputFlags::Chat;
		if(input.sprint) flags |= PlayerInputFlags::Sprint;
		flags |= static_cast<PlayerInputFlags>(static_cast<int>(input.stance) << 6);
		writer.Write(static_cast<uint8_t>(flags));
		writer.Write(static_cast<uint8_t>(input.xmove));
		writer.Write(static_cast<uint8_t>(input.ymove));
	}
	
	static EntityUpdateItem DecodeEntityUpdateItem(PacketReader& reader) {
		EntityUpdateItem item;
		item.entityId = static_cast<uint32_t>(reader.ReadVariableInteger());
		
		auto updates = static_cast<EntityUpdateFlags>(reader.ReadByte());
		
		item.create = updates & EntityUpdateFlags::Create;
		if(item.create) {
			item.type = static_cast<EntityType>(reader.ReadByte());
		}
		
		item.includeFlags = updates & EntityUpdateFlags::Flags;
		if(item.includeFlags) {
			item.flags = FromEntityFlagsValue
			(static_cast<EntityFlagsValue>(reader.ReadByte()));
		}
		
		item.includeTrajectory = updates & EntityUpdateFlags::Trajectory;
		if(item.includeTrajectory) {
			item.trajectory = DecodeTrajectory(reader);
		}
		
		item.includePlayerInput = updates & EntityUpdateFlags::PlayerInput;
		if(item.includePlayerInput) {
			item.playerInput = DecodePlayerInput(reader);
		}
		
		item.includeBlockColor = updates & EntityUpdateFlags::BlockColor;
		if(item.includeBlockColor) {
			item.blockColor = reader.ReadIntColor();
		}
		
		item.includeHealth = updates & EntityUpdateFlags::Health;
		if(item.includeHealth) {
			item.health = reader.ReadByte();
		}
		
		item.includeSkin = updates & EntityUpdateFlags::Skins;
		if(item.includeSkin) {
			item.bodySkin = reader.ReadBytes();
			item.weaponSkin1 = reader.ReadBytes();
			item.weaponSkin2 = reader.ReadBytes();
			item.weaponSkin3 = reader.ReadBytes();
		}
		
		return item;
	}
	
	static void WriteEntityUpdateItem(PacketWriter& writer, const EntityUpdateItem& item) {
		writer.WriteVariableInteger(item.entityId);
		
		auto flags = EntityUpdateFlags::None;
		
		if(item.includeFlags) flags |= EntityUpdateFlags::Flags;
		if(item.includeTrajectory) flags |= EntityUpdateFlags::Trajectory;
		if(item.includePlayerInput) flags |= EntityUpdateFlags::PlayerInput;
		if(item.includeTool) flags |= EntityUpdateFlags::Trajectory;
		if(item.includeBlockColor) flags |= EntityUpdateFlags::BlockColor;
		if(item.includeHealth) flags |= EntityUpdateFlags::Health;
		if(item.includeSkin) flags |= EntityUpdateFlags::Skins;
		
		writer.Write(static_cast<uint8_t>(flags));
		
		if(item.create) {
			writer.Write(static_cast<uint8_t>(item.type));
		}
		
		if(item.includeFlags) {
			writer.Write(static_cast<uint8_t>(ToEntityFlagsValue(item.flags)));
		}
		
		if(item.includeTrajectory) {
			WriteTrajectory(writer, item.trajectory);
		}
		
		if(item.includePlayerInput) {
			WritePlayerInput(writer, item.playerInput);
		}
		
		if(item.includeTool) {
			writer.Write(static_cast<uint8_t>(item.tool));
		}
		
		if(item.includeBlockColor) {
			writer.WriteColor(item.blockColor);
		}
		
		if(item.includeHealth) {
			writer.Write(static_cast<uint8_t>(item.health));
		}
		
		if(item.includeSkin) {
			writer.WriteBytes(item.bodySkin);
			writer.WriteBytes(item.weaponSkin1);
			writer.WriteBytes(item.weaponSkin2);
			writer.WriteBytes(item.weaponSkin3);
		}
		
	}
	
	Packet *EntityUpdatePacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<EntityUpdatePacket> p(new EntityUpdatePacket());
		PacketReader reader(data);
		
		while(!reader.IsEndOfPacket()) {
			p->items.emplace_back(DecodeEntityUpdateItem(reader));
		}
		
		return p.release();
	}
	
	std::vector<char> EntityUpdatePacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		for(const auto& item: items) {
			WriteEntityUpdateItem(writer, item);
		}
		
		return std::move(writer.ToArray());
	}
	
	
	
	Packet *ClientSideEntityUpdatePacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<ClientSideEntityUpdatePacket> p(new ClientSideEntityUpdatePacket());
		PacketReader reader(data);
		
		p->timestamp = reader.ReadTimeStamp();
		
		while(!reader.IsEndOfPacket()) {
			p->items.emplace_back(DecodeEntityUpdateItem(reader));
		}
		
		return p.release();
	}
	
	std::vector<char> ClientSideEntityUpdatePacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.Write(timestamp);
		
		for(const auto& item: items) {
			WriteEntityUpdateItem(writer, item);
		}
		
		return std::move(writer.ToArray());
	}
	
	
	
	Packet *JumpActionPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<JumpActionPacket> p(new JumpActionPacket());
		PacketReader reader(data);
		
		p->timestamp = reader.ReadTimeStamp();
		
		return p.release();
	}
	
	std::vector<char> JumpActionPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.Write(timestamp);
		
		return std::move(writer.ToArray());
	}
	
	
	Packet *ReloadWeaponPacket::Decode(const std::vector<char> &data) {
		SPADES_MARK_FUNCTION();
		
		std::unique_ptr<ReloadWeaponPacket> p(new ReloadWeaponPacket());
		PacketReader reader(data);
		
		p->timestamp = reader.ReadTimeStamp();
		
		return p.release();
	}
	
	std::vector<char> ReloadWeaponPacket::Generate() {
		SPADES_MARK_FUNCTION();
		
		PacketWriter writer(Type);
		
		writer.Write(timestamp);
		
		return std::move(writer.ToArray());
	}
	
	
	
	
} }
