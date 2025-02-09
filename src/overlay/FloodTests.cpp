// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "TCPPeer.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/PeerDoor.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"
#include "overlay/OverlayManager.h"
#include "simulation/Topologies.h"
#include "transactions/TxTests.h"
#include "herder/Herder.h"
#include "ledger/LedgerDelta.h"
#include "herder/HerderImpl.h"

namespace stellar
{
using namespace txtest;

TEST_CASE("Flooding", "[flood][overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation;

    // make closing very slow
    auto cfgGen = []()
    {
        static int cfgNum = 1;
        Config cfg = getTestConfig(cfgNum++);
        cfg.ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = 10000;
        return cfg;
    };

    std::vector<SecretKey> sources;
    std::vector<PublicKey> sourcesPub;
    SequenceNumber expectedSeq =0 ;

    std::vector<std::shared_ptr<Application>> nodes;

    auto test = [&](std::function<void(int)> inject, std::function<bool(std::shared_ptr<Application>)> acked)
    {
        simulation->startAllNodes();

        nodes = simulation->getNodes();
        std::shared_ptr<Application> app0 = nodes[0];

        const int nbTx = 100;

        SecretKey root = getRoot(networkID);
        auto rootA =
            AccountFrame::loadAccount(root.getPublicKey(), app0->getDatabase());

        // directly create a bunch of accounts by cloning the root account (one
        // per tx so that we can easily identify them)
        {
            LedgerEntry gen(rootA->mEntry);
            auto& account = gen.data.account();
            for (int i = 0; i < nbTx; i++)
            {
                sources.emplace_back(SecretKey::random());
                sourcesPub.emplace_back(sources.back().getPublicKey());
                account.accountID = sourcesPub.back();
                auto newAccount = EntryFrame::FromXDR(gen);

                // need to create on all nodes
                for (auto n : nodes)
                {
                    LedgerHeader lh;
                    Database& db = n->getDatabase();
                    LedgerDelta delta(lh, db, false);
                    newAccount->storeAdd(delta, db);
                }
            }
        }

        expectedSeq = getAccountSeqNum(root, *app0) + 1;

        // enough for connections to be made
        simulation->crankForAtLeast(std::chrono::seconds(1), false);

        LOG(DEBUG) << "Injecting work";

        // inject transactions
        for (int i = 0; i < nbTx; i++)
        {
            inject(i);
        }

        LOG(DEBUG) << "Done injecting work";

        auto checkSim = [&]()
        {
            bool res = true;
            for (auto n : simulation->getNodes())
            {
                // done in this order to display full list
                res = acked(n) && res;
            }
            return res;
        };

        // see if the transactions got propagated properly
        simulation->crankUntil(checkSim, std::chrono::seconds(60), true);

        for (auto n : nodes)
        {
            auto& m = n->getMetrics();
            std::stringstream out;
            medida::reporting::ConsoleReporter reporter(m, out);
            for (auto const& kv : m.GetAllMetrics())
            {
                auto& metric = kv.first;
                if (metric.domain() == "overlay")
                {
                    out << metric.domain() << "." << metric.type() << "."
                        << metric.name() << std::endl;
                    kv.second->Process(reporter);
                }
            }
            LOG(DEBUG) << " ~~~~~~ " << n->getConfig().PEER_PORT << " :\n" << out.str();
        }
        REQUIRE(checkSim());
    };

    SECTION("transaction flooding")
    {
        auto injectTransaction = [&](int i)
        {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::random();

            auto tx1 = createCreateAccountTx(networkID, sources[i], dest,
                                             expectedSeq, txAmount);

            // round robin
            auto inApp = nodes[i % nodes.size()];

            // this is basically a modified version of Peer::recvTransaction
            auto msg = tx1->toStellarMessage();
            auto res = inApp->getHerder().recvTransaction(tx1);
            REQUIRE(res == Herder::TX_STATUS_PENDING);
            inApp->getOverlayManager().broadcastMessage(msg);

        };

        auto ackedTransactions = [&](std::shared_ptr<Application> app)
        {
            // checks if an app received all transactions or not
            size_t okCount = 0;
            for (auto const& s : sourcesPub)
            {
                okCount += (app->getHerder().getMaxSeqInPendingTxs(s) ==
                              expectedSeq) ? 1 : 0;
            }
            bool res = okCount == sourcesPub.size();
            LOG(DEBUG) << app->getConfig().PEER_PORT
                << (res ? " OK " : " BEHIND ") << okCount << " / " << sourcesPub.size()
                << " peers: " << app->getOverlayManager().getPeers().size();
            return res;
        };

        SECTION("core")
        {
            SECTION("loopback")
            {
                simulation = Topologies::core(4, .666f, Simulation::OVER_LOOPBACK, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
            SECTION("tcp")
            {
                simulation = Topologies::core(4, .666f, Simulation::OVER_TCP, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
        }

        SECTION("outer nodes")
        {
            SECTION("loopback")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
            SECTION("tcp")
                {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_TCP, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
                }
            }
        }

    SECTION("scp messages flooding")
    {
        // SCP messages depend on
        // a quorum set
        // a valid transaction set

        auto injectSCP = [&](int i)
        {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::random();

            auto tx1 = createCreateAccountTx(networkID, sources[i], dest,
                                             expectedSeq, txAmount);

            // round robin
            auto inApp = nodes[i % nodes.size()];

            // create the transaction set containing this transaction
            auto const& lcl = inApp->getLedgerManager().getLastClosedLedgerHeader();
            TxSetFrame txSet(lcl.hash);
            txSet.add(tx1);
            txSet.sortForHash();
            auto& herder = inApp->getHerder();

            herder.recvTxSet(txSet.getContentsHash(), txSet);

            // build the quorum set used by this message
            // use sources as validators
            SCPQuorumSet qset;
            qset.threshold = 1;
            qset.validators.emplace_back(sourcesPub[i]);
            
            Hash qSetHash = sha256(xdr::xdr_to_opaque(qset));

            herder.recvSCPQuorumSet(qSetHash, qset);

            // build an SCP nomination message for the next ledger

            StellarValue sv(txSet.getContentsHash(), lcl.header.scpValue.closeTime + 1,
                            emptyUpgradeSteps, 0);

            SCPEnvelope envelope;

            auto& st = envelope.statement;
            st.slotIndex = lcl.header.ledgerSeq + 1;
            st.pledges.type(SCP_ST_NOMINATE);
            auto& nom = st.pledges.nominate();
            nom.votes.emplace_back(xdr::xdr_to_opaque(sv));
            nom.quorumSetHash = qSetHash;

            // use the sources to sign the message
            st.nodeID = sourcesPub[i];
            envelope.signature = sources[i].sign(xdr::xdr_to_opaque(
                inApp->getNetworkID(), ENVELOPE_TYPE_SCP, st));

            // inject the message
            herder.recvSCPEnvelope(envelope);

        };

        auto ackedSCP = [&](std::shared_ptr<Application> app)
        {
            // checks if an app received and processed all SCP messages
            size_t okCount = 0;
            auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

            HerderImpl& herder = *static_cast<HerderImpl*>(&app->getHerder());
            auto state = herder.getSCP().getCurrentState(lcl.header.ledgerSeq + 1);
            for (auto const& s : sourcesPub)
            {
                if (std::find_if(state.begin(), state.end(), [&](SCPEnvelope const& e) { return e.statement.nodeID == s; }) != state.end())
                {
                    okCount += 1;
                }
            }
            bool res = okCount == sourcesPub.size();
            LOG(DEBUG) << app->getConfig().PEER_PORT
                << (res ? " OK " : " BEHIND ") << okCount << " / " << sourcesPub.size()
                << " peers: " << app->getOverlayManager().getPeers().size();
            return res;
    };

    SECTION("core")
    {
        SECTION("loopback")
        {
                simulation = Topologies::core(4, .666f, Simulation::OVER_LOOPBACK, networkID, cfgGen);
                test(injectSCP, ackedSCP);
        }
        SECTION("tcp")
        {
                simulation = Topologies::core(4, .666f, Simulation::OVER_TCP, networkID, cfgGen);
                test(injectSCP, ackedSCP);
        }
    }

    SECTION("outer nodes")
    {
        SECTION("loopback")
        {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen);
                test(injectSCP, ackedSCP);
        }
        SECTION("tcp")
        {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_TCP, networkID, cfgGen);
                test(injectSCP, ackedSCP);
        }
        }
    }
}
}
