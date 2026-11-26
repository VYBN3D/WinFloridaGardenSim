-- Native multiplayer save sync, additive migration.
-- This does NOT modify the original game tables, WASM, ESP32 firmware, or old test wallet.
-- Only the allowlisted economy fields are stored. Wi-Fi SSIDs/passwords and the rest of NVS
-- are deliberately excluded.

create table if not exists public.garden_native_saves (
  user_id uuid not null references auth.users(id) on delete cascade,
  game_key text not null check (game_key in ('WinFlorida', 'VYBN')),
  state jsonb not null,
  revision bigint not null default 1 check (revision > 0),
  updated_at timestamptz not null default now(),
  primary key (user_id, game_key),
  constraint garden_native_state_is_object check (jsonb_typeof(state) = 'object')
);

create table if not exists public.garden_native_transfers (
  id uuid primary key default gen_random_uuid(),
  game_key text not null check (game_key in ('WinFlorida', 'VYBN')),
  sender_id uuid not null references auth.users(id) on delete cascade,
  recipient_id uuid not null references auth.users(id) on delete cascade,
  asset_type text not null check (asset_type in (
    'cash', 'seed', 'flower', 'hash',
    'water_pack', 'care_kit', 'pest_kit', 'remediation_kit',
    'bong', 'lighter', 'paper'
  )),
  strain_index smallint,
  amount integer not null check (amount > 0 and amount <= 65535),
  idempotency_key uuid not null,
  created_at timestamptz not null default now(),
  constraint garden_native_transfer_not_self check (sender_id <> recipient_id),
  constraint garden_native_strain_shape check (
    (asset_type in ('seed', 'flower', 'hash') and strain_index is not null and strain_index between 0 and 15)
    or
    (asset_type not in ('seed', 'flower', 'hash') and strain_index is null)
  ),
  constraint garden_native_transfer_once unique (sender_id, idempotency_key)
);

create index if not exists garden_native_transfers_recipient_created
  on public.garden_native_transfers (recipient_id, created_at desc);

alter table public.garden_native_saves enable row level security;
alter table public.garden_native_transfers enable row level security;

drop policy if exists "Players can read their own native game saves" on public.garden_native_saves;
create policy "Players can read their own native game saves"
  on public.garden_native_saves for select to authenticated
  using (auth.uid() = user_id);

drop policy if exists "Players can read native transfers involving them" on public.garden_native_transfers;
create policy "Players can read native transfers involving them"
  on public.garden_native_transfers for select to authenticated
  using (auth.uid() = sender_id or auth.uid() = recipient_id);

grant select on public.garden_native_saves to authenticated;
grant select on public.garden_native_transfers to authenticated;
revoke insert, update, delete on public.garden_native_saves from anon, authenticated;
revoke insert, update, delete on public.garden_native_transfers from anon, authenticated;

-- Validate and whitelist native game-economy fields. All values remain strings,
-- matching the browser NVS bridge's representation.
create or replace function public.garden_native_validate_state(p_state jsonb)
returns jsonb
language plpgsql
immutable
set search_path = public, pg_temp
as $$
declare
  v_credits text;
  v_seed text;
  v_flower text;
  v_hash text;
  v_sq text;
  v_hq text;
  v_raw_flower text;
  v_raw_strain text;
  v_raw_quality text;
  v_water text;
  v_care text;
  v_pest text;
  v_rem text;
  v_bongs text;
  v_lighters text;
  v_papers text;
  v_value bigint;
begin
  if p_state is null or jsonb_typeof(p_state) <> 'object' then
    raise exception 'Native save state must be a JSON object';
  end if;

  v_credits := coalesce(p_state->>'credits', '18');
  v_seed := p_state->>'seedArr';
  v_flower := p_state->>'stashArr';
  v_hash := p_state->>'hashArr';
  v_sq := p_state->>'sqArr';
  v_hq := p_state->>'hqArr';
  v_raw_flower := coalesce(p_state->>'rawFlower', '0');
  v_raw_strain := coalesce(p_state->>'rawStrain', '0');
  v_raw_quality := coalesce(p_state->>'rawQual', '70');
  v_water := coalesce(p_state->>'waterPk', '3');
  v_care := coalesce(p_state->>'careKit', '2');
  v_pest := coalesce(p_state->>'pestKit', '2');
  v_rem := coalesce(p_state->>'remKit', '1');
  v_bongs := coalesce(p_state->>'bongs', '0');
  v_lighters := coalesce(p_state->>'lighters', '0');
  v_papers := coalesce(p_state->>'papers', '0');

  if v_seed is null or length(v_seed) <> 32 or v_seed !~ '^[0-9a-fA-F]{32}$'
     or v_flower is null or length(v_flower) <> 64 or v_flower !~ '^[0-9a-fA-F]{64}$'
     or v_hash is null or length(v_hash) <> 64 or v_hash !~ '^[0-9a-fA-F]{64}$'
     or v_sq is null or length(v_sq) <> 32 or v_sq !~ '^[0-9a-fA-F]{32}$'
     or v_hq is null or length(v_hq) <> 32 or v_hq !~ '^[0-9a-fA-F]{32}$' then
    raise exception 'Native save arrays have an invalid hex format or length';
  end if;

  foreach v_value in array array[
    v_credits::bigint, v_raw_flower::bigint, v_raw_strain::bigint,
    v_raw_quality::bigint, v_water::bigint, v_care::bigint,
    v_pest::bigint, v_rem::bigint, v_bongs::bigint,
    v_lighters::bigint, v_papers::bigint
  ] loop
    if v_value < 0 then raise exception 'Native save values cannot be negative'; end if;
  end loop;

  if v_credits::bigint > 65535 or v_raw_flower::bigint > 65535
     or v_raw_strain::bigint > 15 or v_raw_quality::bigint > 255
     or v_water::bigint > 255 or v_care::bigint > 255
     or v_pest::bigint > 255 or v_rem::bigint > 255
     or v_bongs::bigint > 255 or v_lighters::bigint > 255
     or v_papers::bigint > 255 then
    raise exception 'Native save value exceeds the firmware field limit';
  end if;

  return jsonb_build_object(
    'credits', v_credits, 'seedArr', lower(v_seed),
    'stashArr', lower(v_flower), 'hashArr', lower(v_hash),
    'sqArr', lower(v_sq), 'hqArr', lower(v_hq),
    'rawFlower', v_raw_flower, 'rawStrain', v_raw_strain,
    'rawQual', v_raw_quality, 'waterPk', v_water,
    'careKit', v_care, 'pestKit', v_pest, 'remKit', v_rem,
    'bongs', v_bongs, 'lighters', v_lighters, 'papers', v_papers
  );
end;
$$;

revoke all on function public.garden_native_validate_state(jsonb) from public;
grant execute on function public.garden_native_validate_state(jsonb) to authenticated;

-- First import only. Existing server-side state is never overwritten by an import.
create or replace function public.garden_native_save_import(
  p_game_key text,
  p_state jsonb
)
returns table(native_state jsonb, save_revision bigint, was_created boolean)
language plpgsql
security definer
set search_path = public, auth, pg_temp
as $$
declare
  v_user uuid := auth.uid();
  v_state jsonb;
  v_created boolean := false;
begin
  if v_user is null then raise exception 'Sign in is required'; end if;
  if p_game_key is null or p_game_key not in ('WinFlorida', 'VYBN') then raise exception 'Unknown game'; end if;
  v_state := public.garden_native_validate_state(p_state);

  insert into public.garden_native_saves(user_id, game_key, state, revision)
  values (v_user, p_game_key, v_state, 1)
  on conflict (user_id, game_key) do nothing;

  if found then v_created := true; end if;

  return query
    select s.state, s.revision, v_created
    from public.garden_native_saves s
    where s.user_id = v_user and s.game_key = p_game_key;
end;
$$;

-- Optimistic save update. The client must present the revision it last read.
create or replace function public.garden_native_save_update(
  p_game_key text,
  p_state jsonb,
  p_expected_revision bigint
)
returns table(native_state jsonb, save_revision bigint)
language plpgsql
security definer
set search_path = public, auth, pg_temp
as $$
declare
  v_user uuid := auth.uid();
  v_state jsonb;
begin
  if v_user is null then raise exception 'Sign in is required'; end if;
  if p_game_key is null or p_game_key not in ('WinFlorida', 'VYBN') then raise exception 'Unknown game'; end if;
  if p_expected_revision is null or p_expected_revision < 1 then raise exception 'Invalid save revision'; end if;
  v_state := public.garden_native_validate_state(p_state);

  perform 1 from public.garden_native_saves s
    where s.user_id = v_user and s.game_key = p_game_key
      and s.revision = p_expected_revision
    for update;
  if not found then raise exception 'Native save changed elsewhere. Reload and sync before retrying.'; end if;

  return query
    update public.garden_native_saves s
      set state = v_state,
          revision = case when s.state is distinct from v_state then s.revision + 1 else s.revision end,
          updated_at = case when s.state is distinct from v_state then now() else s.updated_at end
      where s.user_id = v_user and s.game_key = p_game_key
        and s.revision = p_expected_revision
      returning s.state, s.revision;
end;
$$;

-- Atomic transfer. Both users must have imported that game's native save first.
-- Cash, supplies, strain counts, and weighted quality are updated in the game's own
-- NVS format, with the same uint8/uint16 limits used by the firmware.
create or replace function public.garden_native_transfer(
  p_game_key text,
  p_recipient_id uuid,
  p_asset_type text,
  p_amount integer,
  p_strain_index smallint,
  p_idempotency_key uuid
)
returns uuid
language plpgsql
security definer
set search_path = public, auth, pg_temp
as $$
declare
  v_sender uuid := auth.uid();
  v_transfer uuid;
  v_existing public.garden_native_transfers%rowtype;
  v_sender_state jsonb;
  v_recipient_state jsonb;
  v_sender_rev bigint;
  v_recipient_rev bigint;
  v_sender_bytes bytea;
  v_recipient_bytes bytea;
  v_sender_q bytea;
  v_recipient_q bytea;
  v_sender_count integer;
  v_recipient_count integer;
  v_sender_quality integer;
  v_recipient_quality integer;
  v_new_quality integer;
  v_balance integer;
  v_recipient_balance integer;
  v_field text;
  v_sender_value integer;
  v_recipient_value integer;
  v_offset integer;
begin
  if v_sender is null then raise exception 'Sign in is required'; end if;
  if p_game_key is null or p_game_key not in ('WinFlorida', 'VYBN') then raise exception 'Unknown game'; end if;
  if p_recipient_id is null or p_recipient_id = v_sender then raise exception 'Choose another grower'; end if;
  if p_amount is null or p_amount < 1 or p_amount > 65535 then raise exception 'Invalid transfer amount'; end if;
  if p_idempotency_key is null then raise exception 'Transfer request ID is required'; end if;
  if p_asset_type is null or p_asset_type not in (
    'cash', 'seed', 'flower', 'hash',
    'water_pack', 'care_kit', 'pest_kit', 'remediation_kit',
    'bong', 'lighter', 'paper'
  ) then raise exception 'Unsupported native item'; end if;
  if p_asset_type in ('seed', 'flower', 'hash') then
    if p_strain_index is null or p_strain_index < 0 or p_strain_index > 15 then
      raise exception 'Choose a valid strain';
    end if;
  elsif p_strain_index is not null then
    raise exception 'This item does not use a strain';
  end if;

  perform pg_advisory_xact_lock(hashtextextended(v_sender::text || ':' || p_idempotency_key::text, 0));
  select * into v_existing from public.garden_native_transfers
    where sender_id = v_sender and idempotency_key = p_idempotency_key;
  if found then
    if v_existing.recipient_id <> p_recipient_id or v_existing.game_key <> p_game_key
       or v_existing.asset_type <> p_asset_type
       or v_existing.strain_index is distinct from p_strain_index
       or v_existing.amount <> p_amount then
      raise exception 'That request ID was already used for a different transfer';
    end if;
    return v_existing.id;
  end if;

  -- Stable lock order for both save rows.
  perform 1 from public.garden_native_saves
    where game_key = p_game_key and user_id in (v_sender, p_recipient_id)
    order by user_id for update;

  select state, revision into v_sender_state, v_sender_rev
    from public.garden_native_saves where user_id = v_sender and game_key = p_game_key;
  if not found then raise exception 'Your native game save has not been synced'; end if;
  select state, revision into v_recipient_state, v_recipient_rev
    from public.garden_native_saves where user_id = p_recipient_id and game_key = p_game_key;
  if not found then raise exception 'That grower has not synced this game yet'; end if;

  if p_asset_type = 'cash' then
    v_balance := (v_sender_state->>'credits')::integer;
    v_recipient_balance := (v_recipient_state->>'credits')::integer;
    if v_balance < p_amount then raise exception 'Not enough cash'; end if;
    if v_recipient_balance + p_amount > 65535 then raise exception 'Recipient cash is at the game limit'; end if;
    v_sender_state := jsonb_set(v_sender_state, '{credits}', to_jsonb((v_balance-p_amount)::text));
    v_recipient_state := jsonb_set(v_recipient_state, '{credits}', to_jsonb((v_recipient_balance+p_amount)::text));

  elsif p_asset_type in ('seed', 'flower', 'hash') then
    if p_asset_type = 'seed' then
      v_sender_bytes := decode(v_sender_state->>'seedArr', 'hex');
      v_recipient_bytes := decode(v_recipient_state->>'seedArr', 'hex');
      v_offset := p_strain_index;
      v_sender_count := get_byte(v_sender_bytes, v_offset);
      v_recipient_count := get_byte(v_recipient_bytes, v_offset);
      if v_sender_count < p_amount then raise exception 'Not enough seeds of that strain'; end if;
      if v_recipient_count + p_amount > 255 then raise exception 'Recipient seed count is at the game limit'; end if;
      v_sender_bytes := set_byte(v_sender_bytes, v_offset, v_sender_count-p_amount);
      v_recipient_bytes := set_byte(v_recipient_bytes, v_offset, v_recipient_count+p_amount);
      v_sender_state := jsonb_set(v_sender_state, '{seedArr}', to_jsonb(encode(v_sender_bytes, 'hex')));
      v_recipient_state := jsonb_set(v_recipient_state, '{seedArr}', to_jsonb(encode(v_recipient_bytes, 'hex')));
    else
      if p_asset_type = 'flower' then
        v_sender_bytes := decode(v_sender_state->>'stashArr', 'hex');
        v_recipient_bytes := decode(v_recipient_state->>'stashArr', 'hex');
        v_sender_q := decode(v_sender_state->>'sqArr', 'hex');
        v_recipient_q := decode(v_recipient_state->>'sqArr', 'hex');
      else
        v_sender_bytes := decode(v_sender_state->>'hashArr', 'hex');
        v_recipient_bytes := decode(v_recipient_state->>'hashArr', 'hex');
        v_sender_q := decode(v_sender_state->>'hqArr', 'hex');
        v_recipient_q := decode(v_recipient_state->>'hqArr', 'hex');
      end if;
      v_offset := p_strain_index * 2;
      v_sender_count := get_byte(v_sender_bytes, v_offset) + get_byte(v_sender_bytes, v_offset+1) * 256;
      v_recipient_count := get_byte(v_recipient_bytes, v_offset) + get_byte(v_recipient_bytes, v_offset+1) * 256;
      if v_sender_count < p_amount then raise exception 'Not enough jars of that strain'; end if;
      if v_recipient_count + p_amount > 65535 then raise exception 'Recipient jar count is at the game limit'; end if;

      v_sender_quality := get_byte(v_sender_q, p_strain_index);
      v_recipient_quality := get_byte(v_recipient_q, p_strain_index);
      v_new_quality := floor(
        (v_recipient_quality::numeric * v_recipient_count
         + v_sender_quality::numeric * p_amount)
        / (v_recipient_count + p_amount)
      )::integer;

      v_sender_bytes := set_byte(v_sender_bytes, v_offset, (v_sender_count-p_amount) & 255);
      v_sender_bytes := set_byte(v_sender_bytes, v_offset+1, ((v_sender_count-p_amount) >> 8) & 255);
      v_recipient_bytes := set_byte(v_recipient_bytes, v_offset, (v_recipient_count+p_amount) & 255);
      v_recipient_bytes := set_byte(v_recipient_bytes, v_offset+1, ((v_recipient_count+p_amount) >> 8) & 255);
      v_recipient_q := set_byte(v_recipient_q, p_strain_index, v_new_quality);

      if p_asset_type = 'flower' then
        v_sender_state := jsonb_set(v_sender_state, '{stashArr}', to_jsonb(encode(v_sender_bytes, 'hex')));
        v_recipient_state := jsonb_set(v_recipient_state, '{stashArr}', to_jsonb(encode(v_recipient_bytes, 'hex')));
        v_recipient_state := jsonb_set(v_recipient_state, '{sqArr}', to_jsonb(encode(v_recipient_q, 'hex')));
      else
        v_sender_state := jsonb_set(v_sender_state, '{hashArr}', to_jsonb(encode(v_sender_bytes, 'hex')));
        v_recipient_state := jsonb_set(v_recipient_state, '{hashArr}', to_jsonb(encode(v_recipient_bytes, 'hex')));
        v_recipient_state := jsonb_set(v_recipient_state, '{hqArr}', to_jsonb(encode(v_recipient_q, 'hex')));
      end if;
    end if;

  else
    v_field := case p_asset_type
      when 'water_pack' then 'waterPk'
      when 'care_kit' then 'careKit'
      when 'pest_kit' then 'pestKit'
      when 'remediation_kit' then 'remKit'
      when 'bong' then 'bongs'
      when 'lighter' then 'lighters'
      when 'paper' then 'papers'
    end;
    v_sender_value := coalesce((v_sender_state->>v_field)::integer, 0);
    v_recipient_value := coalesce((v_recipient_state->>v_field)::integer, 0);
    if v_sender_value < p_amount then raise exception 'Not enough supplies'; end if;
    if v_recipient_value + p_amount > 255 then raise exception 'Recipient supply count is at the game limit'; end if;
    v_sender_state := jsonb_set(v_sender_state, array[v_field], to_jsonb((v_sender_value-p_amount)::text));
    v_recipient_state := jsonb_set(v_recipient_state, array[v_field], to_jsonb((v_recipient_value+p_amount)::text));
  end if;

  insert into public.garden_native_transfers
    (game_key, sender_id, recipient_id, asset_type, strain_index, amount, idempotency_key)
  values
    (p_game_key, v_sender, p_recipient_id, p_asset_type, p_strain_index, p_amount, p_idempotency_key)
  returning id into v_transfer;

  update public.garden_native_saves
    set state = v_sender_state, revision = v_sender_rev+1, updated_at = now()
    where user_id = v_sender and game_key = p_game_key;
  update public.garden_native_saves
    set state = v_recipient_state, revision = v_recipient_rev+1, updated_at = now()
    where user_id = p_recipient_id and game_key = p_game_key;

  return v_transfer;
end;
$$;

revoke all on function public.garden_native_save_import(text, jsonb) from public;
revoke all on function public.garden_native_save_update(text, jsonb, bigint) from public;
revoke all on function public.garden_native_transfer(text, uuid, text, integer, smallint, uuid) from public;
grant execute on function public.garden_native_save_import(text, jsonb) to authenticated;
grant execute on function public.garden_native_save_update(text, jsonb, bigint) to authenticated;
grant execute on function public.garden_native_transfer(text, uuid, text, integer, smallint, uuid) to authenticated;
