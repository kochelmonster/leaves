// JS wrapper classes for Leaves replication sender/receiver.
// These wrap the C++ ReplicationSenderJS / ReplicationReceiverJS
// exposed by embind as Module.ReplicationSender and Module.ReplicationReceiver.
//
// Usage:
//   import { LeavesReplicationSender, LeavesReplicationReceiver } from './leaves_replication.js';
//   const sender = new LeavesReplicationSender(replDb, Module);
//   const receiver = new LeavesReplicationReceiver(replDb, Module);
//   sender.begin(transport, events);
//   receiver.begin(transport, events);

export class LeavesReplicationSender {
    constructor(replicationDB, Module) {
        this._impl = new Module.ReplicationSender(replicationDB);
    }

    async begin(transport, events) {
        await this._impl.begin(transport, events);
    }

    async onMessageReceived(data) {
        await this._impl.onMessageReceived(data);
    }

    state() {
        return this._impl.state();
    }
}

export class LeavesReplicationReceiver {
    constructor(replicationDB, Module) {
        this._impl = new Module.ReplicationReceiver(replicationDB);
    }

    async begin(transport, events) {
        await this._impl.begin(transport, events);
    }

    async onMessageReceived(data) {
        await this._impl.onMessageReceived(data);
    }

    state() {
        return this._impl.state();
    }
}
