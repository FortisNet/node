//! Independent oracle for the native DashPay proof verifier.
//!
//! Runs the official Dash Platform v4.1.1 proof verifier exactly as the Evo
//! SDK's `getDpnsUsernameByName` does: the gRPC-web reply body is decoded
//! with the official prost types, the query is the DPNS `domain` document
//! query `normalizedLabel == label && normalizedParentDomainName == "dash"`
//! (no limit, no order), and `FromProof<DriveDocumentQuery> for Documents`
//! verifies the GroveDB proof and the Tenderdash commit signature under the
//! supplied quorum public key.
//!
//! Modes:
//!   dashpay-oracle batch CASES.json   -> JSON array of verdicts on stdout
//!   dashpay-oracle blake3             -> hex lines in, BLAKE3 hex lines out
use std::io::{BufRead, Write};
use std::sync::Arc;

use base64::Engine;
use dapi_grpc::platform::v0::GetDocumentsResponse;
use dapi_grpc::platform::VersionedGrpcResponse;
use dapi_grpc::Message;
use dash_context_provider::{ContextProvider, ContextProviderError};
use dpp::data_contract::accessors::v0::DataContractV0Getters;
use dpp::data_contract::TokenConfiguration;
use dpp::document::DocumentV0Getters;
use dpp::platform_value::Value;
use dpp::prelude::{CoreBlockHeight, DataContract, Identifier};
use dpp::system_data_contracts::{load_system_data_contract, SystemDataContract};
use dpp::version::PlatformVersion;
use drive::query::{DriveDocumentQuery, InternalClauses, WhereClause, WhereOperator};
use drive_proof_verifier::types::Documents;
use drive_proof_verifier::FromProof;
use serde_json::{json, Value as Json};

/// The trusted context: one known quorum (any type) or none.
struct Provider {
    quorum: Option<([u8; 32], [u8; 48])>,
}

impl ContextProvider for Provider {
    fn get_data_contract(
        &self,
        _id: &Identifier,
        _platform_version: &PlatformVersion,
    ) -> Result<Option<Arc<DataContract>>, ContextProviderError> {
        Ok(None)
    }

    fn get_token_configuration(
        &self,
        _id: &Identifier,
    ) -> Result<Option<TokenConfiguration>, ContextProviderError> {
        Ok(None)
    }

    fn get_quorum_public_key(
        &self,
        _quorum_type: u32,
        quorum_hash: [u8; 32],
        _core_chain_locked_height: u32,
    ) -> Result<[u8; 48], ContextProviderError> {
        match self.quorum {
            Some((hash, key)) if hash == quorum_hash => Ok(key),
            _ => Err(ContextProviderError::InvalidQuorum("quorum not in the trusted list".into())),
        }
    }

    fn get_platform_activation_height(&self) -> Result<CoreBlockHeight, ContextProviderError> {
        Ok(1)
    }
}

/// gRPC-web: exactly the data frame's message (trailers are ignored here;
/// the native parser checks grpc-status separately).
fn grpc_web_message(body: &[u8]) -> Result<Vec<u8>, String> {
    let mut at = 0;
    let mut message = None;
    while at < body.len() {
        if body.len() - at < 5 {
            return Err("truncated frame header".into());
        }
        let flag = body[at];
        let length = u32::from_be_bytes(body[at + 1..at + 5].try_into().unwrap()) as usize;
        at += 5;
        if body.len() - at < length {
            return Err("truncated frame".into());
        }
        if flag & 0x80 == 0 {
            if flag != 0 || message.is_some() {
                return Err("unexpected data frame".into());
            }
            message = Some(body[at..at + length].to_vec());
        }
        at += length;
    }
    message.ok_or_else(|| "no data frame".into())
}

fn verify_case(case: &Json, contract: &DataContract, version: &PlatformVersion) -> Json {
    let id = case["id"].clone();
    let label = case["label"].as_str().unwrap_or_default().to_string();
    let body = match base64::engine::general_purpose::STANDARD.decode(case["body_b64"].as_str().unwrap_or("")) {
        Ok(bytes) => bytes,
        Err(e) => return json!({"id": id, "ok": false, "error": format!("base64: {e}")}),
    };
    let message = match grpc_web_message(&body) {
        Ok(m) => m,
        Err(e) => return json!({"id": id, "ok": false, "error": format!("framing: {e}")}),
    };
    let response = match GetDocumentsResponse::decode(message.as_slice()) {
        Ok(r) => r,
        Err(e) => return json!({"id": id, "ok": false, "error": format!("decode: {e}")}),
    };
    let quorum = match (case["quorum_hash_hex"].as_str(), case["quorum_key_hex"].as_str()) {
        (Some(hash), Some(key)) => {
            let hash: Option<[u8; 32]> = hex::decode(hash).ok().and_then(|v| v.try_into().ok());
            let key: Option<[u8; 48]> = hex::decode(key).ok().and_then(|v| v.try_into().ok());
            hash.zip(key)
        }
        _ => None,
    };
    let provider = Provider { quorum };
    let document_type = contract.document_type_for_name("domain").expect("domain type");
    let clauses = vec![
        WhereClause {
            field: "normalizedLabel".to_string(),
            operator: WhereOperator::Equal,
            value: Value::Text(label.to_lowercase()),
        },
        WhereClause {
            field: "normalizedParentDomainName".to_string(),
            operator: WhereOperator::Equal,
            value: Value::Text("dash".to_string()),
        },
    ];
    let query = DriveDocumentQuery {
        contract,
        document_type,
        internal_clauses: InternalClauses::extract_from_clauses(clauses).expect("clauses"),
        offset: None,
        limit: None,
        order_by: Default::default(),
        start_at: None,
        start_at_included: false,
        block_time_ms: None,
    };
    // The GroveDB root alone (DriveDocumentQuery::verify_proof), when there is a proof.
    let root = response
        .proof()
        .ok()
        .and_then(|proof| query.verify_proof(&proof.grovedb_proof, version).ok())
        .map(|(root, _)| hex::encode(root));
    match <Documents as FromProof<DriveDocumentQuery>>::maybe_from_proof_with_metadata(
        query.clone(),
        response,
        dpp::dashcore::Network::Mainnet,
        version,
        &provider,
    ) {
        Ok((documents, metadata, _)) => {
            let first = documents.and_then(|d| d.into_iter().next()).and_then(|(_, d)| d);
            json!({
                "id": id,
                "ok": true,
                "root_hash": root,
                "identity_id": first.as_ref().map(|d| bs58::encode(d.owner_id().to_buffer()).into_string()),
                "document_id": first.as_ref().map(|d| bs58::encode(d.id().to_buffer()).into_string()),
                "height": metadata.height.to_string(),
                "time_ms": metadata.time_ms.to_string(),
            })
        }
        Err(e) => json!({"id": id, "ok": false, "root_hash": root, "error": e.to_string()}),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("batch") => {
            let text = std::fs::read_to_string(&args[2]).expect("read cases");
            let input: Json = serde_json::from_str(&text).expect("cases json");
            // The SDK verifies with its current platform version: 13 once it
            // has seen a mainnet reply (protocol 13 at capture time).
            let version = PlatformVersion::get(13).expect("platform version 13");
            let contract = load_system_data_contract(SystemDataContract::DPNS, version).expect("dpns");
            assert_eq!(
                contract.id().to_string(dpp::platform_value::string_encoding::Encoding::Base58),
                "GWRSAVFMjXx8HpQFaNJMqBV7MBgMK4br5UESsB4S31Ec"
            );
            let verdicts: Vec<Json> = input["cases"]
                .as_array()
                .expect("cases array")
                .iter()
                .map(|case| verify_case(case, &contract, version))
                .collect();
            println!("{}", serde_json::to_string(&verdicts).unwrap());
        }
        Some("blake3") => {
            let stdin = std::io::stdin();
            let mut stdout = std::io::stdout();
            for line in stdin.lock().lines() {
                let bytes = hex::decode(line.expect("line").trim()).expect("hex");
                writeln!(stdout, "{}", blake3::hash(&bytes).to_hex()).unwrap();
                stdout.flush().unwrap();
            }
        }
        _ => {
            eprintln!("usage: dashpay-oracle batch CASES.json | dashpay-oracle blake3");
            std::process::exit(2);
        }
    }
}
