#pragma once
#include <array>
#include <cstdint>
namespace jfg {
// Optional ambient sound must not follow an invalid or cyclic actor chain.
// Valid groups still use the original selector and sound implementation.
template<class Memory> bool boss_ambient_chain_valid(const Memory& memory,std::uint32_t group) {
 if(!memory.valid(group,0x88U))return false;
 auto current=memory.u32(group+0x58U);
 std::array<std::uint32_t,256> visited{};
 std::size_t count=0;
 while(current) {
  if((current&3U)!=0 || !memory.valid(current,0x6CU) || count==visited.size())return false;
  for(std::size_t i=0;i<count;++i)if(visited[i]==current)return false;
  visited[count++]=current;
  const auto control=memory.u32(current+0x68U);
  if((control&3U)!=0 || !memory.valid(control,0xAAU))return false;
  current=memory.u32(control+0x28U);
 }
 return true;
}
}

namespace jfg {
struct SquadUnlink {
 std::uint32_t link=0U,next=0U;
};
// A squad's live count can reach zero before its dying actors are released.
// Locate the owning link by following the actual chain, never that count.
// Return a plan only after validating the complete chain, including the tail.
template<class Memory> SquadUnlink squad_release_link(const Memory& memory,
    std::uint32_t group,std::uint32_t actor) {
 if((group&3U)!=0U || !memory.valid(group,0x88U))return {};
 std::array<std::uint32_t,256> seen{};
 std::size_t count=0U;
 std::uint32_t link=group+0x58U,current=memory.u32(link);
 SquadUnlink plan{};
 while(current!=0U) {
  if((current&3U)!=0U || !memory.valid(current,0x6CU) || count==seen.size())return {};
  for(std::size_t i=0;i<count;++i)if(seen[i]==current)return {};
  seen[count++]=current;
  const auto control=memory.u32(current+0x68U);
  if((control&3U)!=0U || !memory.valid(control,0xAAU))return {};
  const auto next=memory.u32(control+0x28U);
  if(current==actor)plan={link,next};
  link=control+0x28U;current=next;
 }
 return plan;
}
}
