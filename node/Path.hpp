/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2016  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ZT_PATH_HPP
#define ZT_PATH_HPP

#include <stdint.h>
#include <string.h>

#include <stdexcept>
#include <algorithm>

#include "Constants.hpp"
#include "InetAddress.hpp"
#include "SharedPtr.hpp"
#include "AtomicCounter.hpp"

/**
 * Maximum return value of preferenceRank()
 */
#define ZT_PATH_MAX_PREFERENCE_RANK ((ZT_INETADDRESS_MAX_SCOPE << 1) | 1)

namespace ZeroTier {

class RuntimeEnvironment;

/**
 * A path across the physical network
 */
class Path
{
	friend class SharedPtr<Path>;

public:
	/**
	 * Efficient unique key for paths in a Hashtable
	 */
	class HashKey
	{
	public:
		HashKey() {}

		HashKey(const InetAddress &l,const InetAddress &r)
		{
			// This is an ad-hoc bit packing algorithm to yield unique keys for
			// remote addresses and their local-side counterparts if defined.
			// Portability across runtimes is not needed.
			if (r.ss_family == AF_INET) {
				_k[0] = (uint64_t)reinterpret_cast<const struct sockaddr_in *>(&r)->sin_addr.s_addr;
				_k[1] = (uint64_t)reinterpret_cast<const struct sockaddr_in *>(&r)->sin_port;
				if (l.ss_family == AF_INET) {
					_k[2] = (uint64_t)reinterpret_cast<const struct sockaddr_in *>(&l)->sin_addr.s_addr;
					_k[3] = (uint64_t)reinterpret_cast<const struct sockaddr_in *>(&r)->sin_port;
				} else {
					_k[2] = 0;
					_k[3] = 0;
				}
			} else if (r.ss_family == AF_INET6) {
				const uint8_t *a = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&r)->sin6_addr.s6_addr);
				uint8_t *b = reinterpret_cast<uint8_t *>(_k);
				for(unsigned int i=0;i<16;++i) b[i] = a[i];
				_k[2] = ~((uint64_t)reinterpret_cast<const struct sockaddr_in6 *>(&r)->sin6_port);
				if (l.ss_family == AF_INET6) {
					_k[2] ^= ((uint64_t)reinterpret_cast<const struct sockaddr_in6 *>(&r)->sin6_port) << 32;
					a = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&l)->sin6_addr.s6_addr);
					b += 24;
					for(unsigned int i=0;i<8;++i) b[i] = a[i];
					a += 8;
					for(unsigned int i=0;i<8;++i) b[i] ^= a[i];
				}
			} else {
				_k[0] = 0;
				_k[1] = 0;
				_k[2] = 0;
				_k[3] = 0;
			}
		}

		inline unsigned long hashCode() const { return (unsigned long)(_k[0] + _k[1] + _k[2] + _k[3]); }

		inline bool operator==(const HashKey &k) const { return ( (_k[0] == k._k[0]) && (_k[1] == k._k[1]) && (_k[2] == k._k[2]) && (_k[3] == k._k[3]) ); }
		inline bool operator!=(const HashKey &k) const { return (!(*this == k)); }

	private:
		uint64_t _k[4];
	};

	Path() :
		_lastOut(0),
		_lastIn(0),
		_addr(),
		_localAddress(),
		_ipScope(InetAddress::IP_SCOPE_NONE),
		_clusterSuboptimal(false)
	{
	}

	Path(const InetAddress &localAddress,const InetAddress &addr) :
		_lastOut(0),
		_lastIn(0),
		_addr(addr),
		_localAddress(localAddress),
		_ipScope(addr.ipScope()),
		_clusterSuboptimal(false)
	{
	}

	inline Path &operator=(const Path &p)
	{
		if (this != &p)
			memcpy(this,&p,sizeof(Path));
		return *this;
	}

	/**
	 * Called when a packet is sent to this remote path
	 *
	 * This is called automatically by Path::send().
	 *
	 * @param t Time of send
	 */
	inline void sent(const uint64_t t) { _lastOut = t; }

	/**
	 * Called when a packet is received from this remote path, regardless of content
	 *
	 * @param t Time of receive
	 */
	inline void received(const uint64_t t) { _lastIn = t; }

	/**
	 * Send a packet via this path (last out time is also updated)
	 *
	 * @param RR Runtime environment
	 * @param data Packet data
	 * @param len Packet length
	 * @param now Current time
	 * @return True if transport reported success
	 */
	bool send(const RuntimeEnvironment *RR,const void *data,unsigned int len,uint64_t now);

	/**
	 * @return Address of local side of this path or NULL if unspecified
	 */
	inline const InetAddress &localAddress() const throw() { return _localAddress; }

	/**
	 * @return Physical address
	 */
	inline const InetAddress &address() const throw() { return _addr; }

	/**
	 * @return IP scope -- faster shortcut for address().ipScope()
	 */
	inline InetAddress::IpScope ipScope() const throw() { return _ipScope; }

	/**
	 * @param f Is this path cluster-suboptimal?
	 */
	inline void setClusterSuboptimal(const bool f) { _clusterSuboptimal = f; }

	/**
	 * @return True if cluster-suboptimal (for someone)
	 */
	inline bool isClusterSuboptimal() const { return _clusterSuboptimal; }

	/**
	 * @return True if cluster-optimal (for someone) (the default)
	 */
	inline bool isClusterOptimal() const { return (!(_clusterSuboptimal)); }

	/**
	 * @return Preference rank, higher == better (will be less than 255)
	 */
	inline unsigned int preferenceRank() const throw()
	{
		/* First, since the scope enum values in InetAddress.hpp are in order of
		 * use preference rank, we take that. Then we multiple by two, yielding
		 * a sequence like 0, 2, 4, 6, etc. Then if it's IPv6 we add one. This
		 * makes IPv6 addresses of a given scope outrank IPv4 addresses of the
		 * same scope -- e.g. 1 outranks 0. This makes us prefer IPv6, but not
		 * if the address scope/class is of a fundamentally lower rank. */
		return ( ((unsigned int)_ipScope << 1) | (unsigned int)(_addr.ss_family == AF_INET6) );
	}

	/**
	 * @return This path's overall quality score (higher is better)
	 */
	inline uint64_t score() const throw()
	{
		// This is a little bit convoluted because we try to be branch-free, using multiplication instead of branches for boolean flags

		// Start with the last time this path was active, and add a fudge factor to prevent integer underflow if _lastReceived is 0
		uint64_t score = _lastIn + (ZT_PEER_DIRECT_PING_DELAY * (ZT_PEER_DEAD_PATH_DETECTION_MAX_PROBATION + 1));

		// Increase score based on path preference rank, which is based on IP scope and address family
		score += preferenceRank() * (ZT_PEER_DIRECT_PING_DELAY / ZT_PATH_MAX_PREFERENCE_RANK);

		// Decrease score if this is known to be a sub-optimal path to a cluster
		score -= ((uint64_t)_clusterSuboptimal) * ZT_PEER_DIRECT_PING_DELAY;

		return score;
	}

	/**
	 * Check whether this address is valid for a ZeroTier path
	 *
	 * This checks the address type and scope against address types and scopes
	 * that we currently support for ZeroTier communication.
	 *
	 * @param a Address to check
	 * @return True if address is good for ZeroTier path use
	 */
	static inline bool isAddressValidForPath(const InetAddress &a)
		throw()
	{
		if ((a.ss_family == AF_INET)||(a.ss_family == AF_INET6)) {
			switch(a.ipScope()) {
				/* Note: we don't do link-local at the moment. Unfortunately these
				 * cause several issues. The first is that they usually require a
				 * device qualifier, which we don't handle yet and can't portably
				 * push in PUSH_DIRECT_PATHS. The second is that some OSes assign
				 * these very ephemerally or otherwise strangely. So we'll use
				 * private, pseudo-private, shared (e.g. carrier grade NAT), or
				 * global IP addresses. */
				case InetAddress::IP_SCOPE_PRIVATE:
				case InetAddress::IP_SCOPE_PSEUDOPRIVATE:
				case InetAddress::IP_SCOPE_SHARED:
				case InetAddress::IP_SCOPE_GLOBAL:
					if (a.ss_family == AF_INET6) {
						// TEMPORARY HACK: for now, we are going to blacklist he.net IPv6
						// tunnels due to very spotty performance and low MTU issues over
						// these IPv6 tunnel links.
						const uint8_t *ipd = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_addr.s6_addr);
						if ((ipd[0] == 0x20)&&(ipd[1] == 0x01)&&(ipd[2] == 0x04)&&(ipd[3] == 0x70))
							return false;
					}
					return true;
				default:
					return false;
			}
		}
		return false;
	}

private:
	uint64_t _lastOut;
	uint64_t _lastIn;
	InetAddress _addr;
	InetAddress _localAddress;
	InetAddress::IpScope _ipScope; // memoize this since it's a computed value checked often
	AtomicCounter __refCount;
	bool _clusterSuboptimal;
};

} // namespace ZeroTier

#endif
