fx_log("running");
while (fx_running()) {
    bool hit = fx_key_pressed('G');
    if (hit) {
        uint64_t chr = fx_character(); // get chr, print no chr if value returned 0
        if (!chr) {
            fx_log("no chr");
            continue;
        } // gate if chr 0
        uint64_t root = fx_part(chr, "HumanoidRootPart"); // get hrp
        if (!root) { 
            fx_log("no hrp");
            continue; 
        } // same guard shape as chr
        float3 p{};
        if (!fx_pos(root, &p)) { // get pos
            fx_log("pos read failed"); // print if fail
            continue; 
        }
        fx_logf("pos: %.2f, %.2f, %.2f", p.x, p.y, p.z); // print pos in float3
    }
    if (!fx_wait_key(200)) break;   // wakes on a key, on Stop, or after 200ms; 0 means Stop
}