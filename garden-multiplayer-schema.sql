-- Garden multiplayer economy foundation
-- Draft only. Review and apply manually in Supabase after enabling the intended Auth mode.
-- This is a separate multiplayer economy; it does not modify either game's WASM save data.

create table if not exists public.garden_player_profiles (
  user_id uuid primary key references auth.users(id) on delete cascade,
  display_name text not null default 'Grower'
    check (char_length(display_name) between 1 and 24),
  created_at timestamptz not null default now()
);

create table if not exists public.garden_wallets (
  user_id uuid primary key references auth.users(id) on delete cascade,
  cash_balance bigint not null default 20
    check (cash_balance >= 0 and cash_balance <= 9000000000000000),
  updated_at timestamptz not null default now()
);

create table if not exists public.garden_inventory (
  user_id uuid not null references auth.users(id) on delete cascade,
  item_key text not null
    check (item_key in ('supply', 'flower_jar', 'hash_jar')),
  quantity bigint not null default 0
    check (quantity >= 0 and quantity <= 9000000000000000),
  updated_at timestamptz not null default now(),
  primary key (user_id, item_key)
);

create table if not exists public.garden_transfers (
  id uuid primary key default gen_random_uuid(),
  sender_id uuid not null references auth.users(id),
  recipient_id uuid not null references auth.users(id),
  asset_type text not null check (asset_type in ('cash', 'item')),
  item_key text,
  amount bigint not null check (amount > 0 and amount <= 1000000),
  idempotency_key uuid not null,
  created_at timestamptz not null default now(),
  constraint garden_transfer_not_self check (sender_id <> recipient_id),
  constraint garden_transfer_asset_shape check (
    (asset_type = 'cash' and item_key is null) or
    (asset_type = 'item' and item_key is not null and item_key in ('supply', 'flower_jar', 'hash_jar'))
  ),
  constraint garden_transfer_once unique (sender_id, idempotency_key)
);

create index if not exists garden_transfers_recipient_created
  on public.garden_transfers (recipient_id, created_at desc);

create or replace function public.garden_on_user_created()
returns trigger
language plpgsql
security definer
set search_path = public, pg_temp
as $$
begin
  insert into public.garden_player_profiles (user_id)
  values (new.id) on conflict (user_id) do nothing;
  insert into public.garden_wallets (user_id)
  values (new.id) on conflict (user_id) do nothing;
  return new;
end;
$$;

drop trigger if exists garden_auth_user_created on auth.users;
create trigger garden_auth_user_created
  after insert on auth.users
  for each row execute function public.garden_on_user_created();

-- Backfill accounts that already exist before this migration is applied.
insert into public.garden_player_profiles (user_id)
select id from auth.users on conflict (user_id) do nothing;
insert into public.garden_wallets (user_id)
select id from auth.users on conflict (user_id) do nothing;

alter table public.garden_player_profiles enable row level security;
alter table public.garden_wallets enable row level security;
alter table public.garden_inventory enable row level security;
alter table public.garden_transfers enable row level security;

drop policy if exists "Authenticated users can view grower profiles" on public.garden_player_profiles;
create policy "Authenticated users can view grower profiles"
  on public.garden_player_profiles for select to authenticated using (true);

drop policy if exists "Players can view their own wallet" on public.garden_wallets;
create policy "Players can view their own wallet"
  on public.garden_wallets for select to authenticated using (auth.uid() = user_id);

drop policy if exists "Players can view their own inventory" on public.garden_inventory;
create policy "Players can view their own inventory"
  on public.garden_inventory for select to authenticated using (auth.uid() = user_id);

drop policy if exists "Players can view transfers involving them" on public.garden_transfers;
create policy "Players can view transfers involving them"
  on public.garden_transfers for select to authenticated
  using (auth.uid() = sender_id or auth.uid() = recipient_id);

create or replace function public.garden_set_display_name(p_display_name text)
returns void
language plpgsql
security definer
set search_path = public, auth, pg_temp
as $$
declare
  v_user_id uuid := auth.uid();
  v_name text := btrim(coalesce(p_display_name, ''));
begin
  if v_user_id is null then
    raise exception 'Sign in is required';
  end if;
  if char_length(v_name) < 1 or char_length(v_name) > 24 then
    raise exception 'Grower name must be 1 to 24 characters';
  end if;
  insert into public.garden_player_profiles (user_id, display_name)
  values (v_user_id, v_name)
  on conflict (user_id) do update
    set display_name = excluded.display_name;
end;
$$;

revoke all on function public.garden_set_display_name(text) from public;
grant execute on function public.garden_set_display_name(text) to authenticated;

grant select on public.garden_player_profiles to authenticated;
grant select on public.garden_wallets to authenticated;
grant select on public.garden_inventory to authenticated;
grant select on public.garden_transfers to authenticated;

-- No client INSERT/UPDATE/DELETE policies are intentionally provided.
-- All balance mutations must go through this atomic, identity-bound RPC.
create or replace function public.garden_transfer_asset(
  p_recipient_id uuid,
  p_asset_type text,
  p_amount bigint,
  p_item_key text,
  p_idempotency_key uuid
)
returns uuid
language plpgsql
security definer
set search_path = public, auth, pg_temp
as $$
declare
  v_sender_id uuid := auth.uid();
  v_transfer_id uuid;
  v_existing public.garden_transfers%rowtype;
  v_wallet_balance bigint;
  v_item_quantity bigint;
begin
  if v_sender_id is null then
    raise exception 'Sign in is required';
  end if;
  if p_recipient_id is null or p_recipient_id = v_sender_id then
    raise exception 'Choose another grower';
  end if;
  if p_amount is null or p_amount < 1 or p_amount > 1000000 then
    raise exception 'Amount must be between 1 and 1000000';
  end if;
  if p_idempotency_key is null then
    raise exception 'A transfer request ID is required';
  end if;
  if p_asset_type is null or p_asset_type not in ('cash', 'item') then
    raise exception 'Unsupported asset type';
  end if;
  if p_asset_type = 'cash' and p_item_key is not null then
    raise exception 'Cash transfers cannot include an item key';
  end if;
  if p_asset_type = 'item' and (p_item_key is null or p_item_key not in ('supply', 'flower_jar', 'hash_jar')) then
    raise exception 'Unsupported item type';
  end if;
  if not exists (select 1 from public.garden_player_profiles where user_id = p_recipient_id) then
    raise exception 'Recipient profile was not found';
  end if;

  -- Serialize transfers between this pair to avoid deadlocks and balance races.
  perform pg_advisory_xact_lock(hashtextextended(
    least(v_sender_id::text, p_recipient_id::text) || ':' ||
    greatest(v_sender_id::text, p_recipient_id::text), 0
  ));

  -- Idempotency: a retried request returns its original receipt, never transfers twice.
  select * into v_existing
    from public.garden_transfers
    where sender_id = v_sender_id and idempotency_key = p_idempotency_key;
  if found then
    if v_existing.recipient_id <> p_recipient_id
       or v_existing.asset_type <> p_asset_type
       or v_existing.item_key is distinct from p_item_key
       or v_existing.amount <> p_amount then
      raise exception 'That request ID was already used for a different transfer';
    end if;
    return v_existing.id;
  end if;

  insert into public.garden_wallets (user_id)
  values (v_sender_id), (p_recipient_id)
  on conflict (user_id) do nothing;

  perform 1 from public.garden_wallets
    where user_id in (v_sender_id, p_recipient_id)
    order by user_id for update;

  if p_asset_type = 'cash' then
    select cash_balance into v_wallet_balance
      from public.garden_wallets where user_id = v_sender_id;
    if v_wallet_balance < p_amount then
      raise exception 'Not enough cash';
    end if;
  else
    insert into public.garden_inventory (user_id, item_key)
    values (v_sender_id, p_item_key), (p_recipient_id, p_item_key)
    on conflict (user_id, item_key) do nothing;

    perform 1 from public.garden_inventory
      where item_key = p_item_key
        and user_id in (v_sender_id, p_recipient_id)
      order by user_id for update;

    select quantity into v_item_quantity
      from public.garden_inventory
      where user_id = v_sender_id and item_key = p_item_key;
    if v_item_quantity < p_amount then
      raise exception 'Not enough items';
    end if;
  end if;

  insert into public.garden_transfers
    (sender_id, recipient_id, asset_type, item_key, amount, idempotency_key)
  values
    (v_sender_id, p_recipient_id, p_asset_type, p_item_key, p_amount, p_idempotency_key)
  returning id into v_transfer_id;

  if p_asset_type = 'cash' then
    update public.garden_wallets
      set cash_balance = cash_balance - p_amount, updated_at = now()
      where user_id = v_sender_id;
    update public.garden_wallets
      set cash_balance = cash_balance + p_amount, updated_at = now()
      where user_id = p_recipient_id;
  else
    update public.garden_inventory
      set quantity = quantity - p_amount, updated_at = now()
      where user_id = v_sender_id and item_key = p_item_key;
    update public.garden_inventory
      set quantity = quantity + p_amount, updated_at = now()
      where user_id = p_recipient_id and item_key = p_item_key;
  end if;

  return v_transfer_id;
end;
$$;

revoke all on function public.garden_transfer_asset(uuid, text, bigint, text, uuid) from public;
grant execute on function public.garden_transfer_asset(uuid, text, bigint, text, uuid) to authenticated;

revoke insert, update, delete on public.garden_player_profiles from anon, authenticated;
revoke insert, update, delete on public.garden_wallets from anon, authenticated;
revoke insert, update, delete on public.garden_inventory from anon, authenticated;
revoke insert, update, delete on public.garden_transfers from anon, authenticated;
